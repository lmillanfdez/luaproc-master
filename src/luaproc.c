/*
** luaproc API
** See Copyright Notice in luaproc.h
*/

#include <pthread.h>
#include <stdlib.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <unistd.h> /* close */
#include <string.h> /* memset */
#include <time.h>

#include "luaproc.h"
#include "lpsched.h"
#include "udata.h"

#define FALSE 0
#define TRUE  !FALSE
#define LUAPROC_CHANNELS_TABLE "channeltb"
#define LUAPROC_RECYCLE_MAX 0

//name of the blocking userdata metatable
#define LUAPROC_DENIED_MTUDATA "denied_mtudata"

//name of the metatable indicating that a table in a container Lua state represents the table registered by a C module
#define MT_MODULE "luaproc_mt_module"

//name of the metatable indicating that a table in a container Lua state represents the path to locate a C function
#define MT_CFUNCTION "luaproc_mt_cfunction"

//max number of nesting levels that a table may have in message exchange
#define MAX_NESTING_LEVELS 250

#if (LUA_VERSION_NUM == 501)

#define lua_rawlen(L, index)	lua_objlen(L, index)

#define lua_pushglobaltable( L )    lua_pushvalue( L, LUA_GLOBALSINDEX )
#define luaL_newlib( L, funcs )     { lua_newtable( L ); \
  luaL_register( L, NULL, funcs ); }
#define isequal( L, a, b )          lua_equal( L, a, b )
#define requiref( L, modname, f, glob ) {\
  lua_pushcfunction( L, f ); /* push module load function */ \
  lua_pushstring( L, modname );  /* argument to module load function */ \
  lua_call( L, 1, 1 );  /* call 'f' to load module */ \
  /* register module in package.loaded table in case 'f' doesn't do so */ \
  lua_getfield( L, LUA_GLOBALSINDEX, LUA_LOADLIBNAME );\
  if ( lua_type( L, -1 ) == LUA_TTABLE ) {\
    lua_getfield( L, -1, "loaded" );\
    if ( lua_type( L, -1 ) == LUA_TTABLE ) {\
      lua_getfield( L, -1, modname );\
      if ( lua_type( L, -1 ) == LUA_TNIL ) {\
        lua_pushvalue( L, 1 );\
        lua_setfield( L, -3, modname );\
      }\
      lua_pop( L, 1 );\
    }\
    lua_pop( L, 1 );\
  }\
  lua_pop( L, 1 );\
  if ( glob ) { /* set global name? */ \
    lua_setglobal( L, modname );\
  } else {\
    lua_pop( L, 1 );\
  }\
}

#else

#define isequal( L, a, b )                 lua_compare( L, a, b, LUA_OPEQ )
#define requiref( L, modname, f, glob ) \
  { luaL_requiref( L, modname, f, glob ); lua_pop( L, 1 ); }

#endif

#if (LUA_VERSION_NUM >= 503)
#define dump( L, writer, data, strip )     lua_dump( L, writer, data, strip )
#define copynumber( Lto, Lfrom, i ) {\
  if ( lua_isinteger( Lfrom, i )) {\
    lua_pushinteger( Lto, lua_tonumber( Lfrom, i ));\
  } else {\
    lua_pushnumber( Lto, lua_tonumber( Lfrom, i ));\
  }\
}

#else
#define dump( L, writer, data, strip )     lua_dump( L, writer, data )
#define copynumber( Lto, Lfrom, i ) \
  lua_pushnumber( Lto, lua_tonumber( Lfrom, i ))
#endif

/********************
 * global variables *
 *******************/

/* channel list mutex */
static pthread_mutex_t mutex_channel_list = PTHREAD_MUTEX_INITIALIZER;

/* recycle list mutex */
static pthread_mutex_t mutex_recycle_list = PTHREAD_MUTEX_INITIALIZER;

/* recycled lua process list */
static list recycle_list;

/* maximum lua processes to recycle */
static int recyclemax = LUAPROC_RECYCLE_MAX;

/* lua_State used to store channel hash table */
static lua_State *chanls = NULL;

/* lua process used to wrap main state. allows main state to be queued in 
   channels when sending and receiving messages */
static luaproc mainlp;

/* main state matched a send/recv operation conditional variable */
pthread_cond_t cond_mainls_sendrecv = PTHREAD_COND_INITIALIZER;

/* main state communication mutex */
static pthread_mutex_t mutex_mainls = PTHREAD_MUTEX_INITIALIZER;


/* key of the table used for storing transferred C functions*/
static const char *func_path = "func_path";

/* key of the table used for storing lua function temporary*/
static const char *func_cache = "func_cache";

/* key of the counter holding the number of levels in recursive calls*/
static const char *recursive_counter = "recursive_counter";

/* key of the table used for storing userdata metatables and their corresponding transfer functions*/
static const char *transferable_udata = "transferable_udata";


/***********
 * enums *
 ***********/
 
 enum key_value{
	asValue,
	asKey
};

//kinds of message transfers
enum t_transfer{
	to_normal,//the message is transferred to a Lua state
	from_normal,//the message is transferred from a Lua state
	to_temp,//the message is transferred to a container Lua state
	from_temp//the message is transferred from a container Lua state
};


/***********************
 * register prototypes *
 ***********************/

static void luaproc_openlualibs( lua_State *L );
static int luaproc_create_newproc( lua_State *L );
static int luaproc_wait( lua_State *L );
static int luaproc_send( lua_State *L );
static int luaproc_receive( lua_State *L );
static int luaproc_create_channel( lua_State *L );
static int luaproc_destroy_channel( lua_State *L );
static int luaproc_set_numworkers( lua_State *L );
static int luaproc_get_numworkers( lua_State *L );
static int luaproc_recycle_set( lua_State *L );
LUALIB_API int luaopen_luaproc( lua_State *L );
static int luaproc_loadlib( lua_State *L ); 

//functions for transferring the table, userdata and function data type
static int transferTable(lua_State *Lfrom, int index, lua_State *Lto, enum t_transfer type_, int lv);
static int transferUdata(lua_State *Lfrom, int index, lua_State *Lto, enum t_transfer type_);
static int transferFunction(lua_State *Lfrom, int index, lua_State *Lto, enum t_transfer type_);
static int transferCFunction(lua_State *Lfrom, int index, lua_State *Lto, enum t_transfer type_);
static int luaproc_default_send_udata(lua_State *L);
static int luaproc_default_recv_udata(lua_State *L);

//functions associated to userdata
static int luaproc_regudata(lua_State *L);
static int luaproc_err_udata (lua_State *L);
static int luaproc_denied_udata (lua_State *L);
static int luaproc_transf_funcs(lua_State *L);

//performs a barrier operation on a specified channel
static int luaproc_barrier(lua_State *L);

/***********
 * structs *
 ***********/

//structure for handling a barrier operation
struct stbarrier {
	//queue storing Lua processes involved in the operation
	list elems;
	//stores the main Lua process (if it is involved in the operation)
	luaproc *mainlp;
	//number of Lua process involved in the operation
	int num_elem;
};

/* lua process */
struct stluaproc {
	lua_State *lstate;
	int status;
	int args;
	channel *chan;
	luaproc *next;
};

/* communication channel */
struct stchannel {
	//indicates the channel's type (0: sync, 1: async)
	int type;
	
	//in async channels, it stores the container Lua state
	lua_State *lstate;
	
	list send;
	list recv;

	//stores the structure defined for handling barrier operation, in case such an operation to be performed on this channel
	struct stbarrier *barrier;
	
	pthread_mutex_t mutex;
	pthread_cond_t can_be_used;
};


/***********
 * exported functions *
 ***********/

/* luaproc function registration array */
static const struct luaL_Reg luaproc_funcs[] = {
	{ "newproc", luaproc_create_newproc },
	{ "wait", luaproc_wait },
	{ "send", luaproc_send },
	{ "receive", luaproc_receive },
	{ "newchannel", luaproc_create_channel },
	{ "delchannel", luaproc_destroy_channel },
	{ "setnumworkers", luaproc_set_numworkers },
	{ "getnumworkers", luaproc_get_numworkers },
	{ "recycle", luaproc_recycle_set },

	{"regudata", luaproc_regudata},
	{"barrier", luaproc_barrier},
	{"transffuncs", luaproc_transf_funcs},
	{ NULL, NULL }
};

/******************
 * list functions *
 ******************/

/* insert a lua process in a (fifo) list */
void list_insert( list *l, luaproc *lp ) {
  if ( l->head == NULL ) {
    l->head = lp;
  } else {
    l->tail->next = lp;
  }
  l->tail = lp;
  lp->next = NULL;
  l->nodes++;
}

//given two lists, it returns a new list, resulting from appending the right list to the left one
void list_join(list *left, list *right) {
  if ( left->head == NULL ) {
    left->head = right->head;
  } else {
    left->tail->next = right->head;
  }
  left->tail = right->tail;
  right->tail->next = NULL;
  left->nodes += right->nodes;
}

/* remove and return the first lua process in a (fifo) list */
luaproc *list_remove( list *l ) {
  if ( l->head != NULL ) {
    luaproc *lp = l->head;
    l->head = lp->next;
    l->nodes--;
    return lp;
  } else {
    return NULL; /* if list is empty, return NULL */
  }
}

/* return a list's node count */
int list_count( list *l ) {
  return l->nodes;
}

/* initialize an empty list */
void list_init( list *l ) {
  l->head = NULL;
  l->tail = NULL;
  l->nodes = 0;
}

/*********************
 * channel functions *
 *********************/

/* create a new channel (sync or async) and insert it into channels table */
static channel *channel_create( const char *cname, int type_ch) {

	channel *chan;

	/* get exclusive access to channels list */
	pthread_mutex_lock( &mutex_channel_list );

	/* create new channel and register its name */
	lua_getglobal( chanls, LUAPROC_CHANNELS_TABLE );
	chan = (channel *)lua_newuserdata( chanls, sizeof( channel ));
	lua_setfield( chanls, -2, cname );
	lua_pop( chanls, 1 );  /* remove channel table from stack */

	/* initialize channel struct */
	
	//establishing the channel's type ("type_ch" may be: 0 - sync or 1 - async)
	chan->type = type_ch;
	
	//initializes a queue for storing Lua processes sending message, for sync channels
	if(!type_ch){
		list_init( &chan->send );
	}
	else{
		//for async channels, create a container Lua state
		chan->lstate = luaL_newstate();
	}
	
	list_init( &chan->recv );

	//this pointer stores a structure only when the barrier operation is being performed
	chan->barrier = NULL;
	
	pthread_mutex_init( &chan->mutex, NULL );
	pthread_cond_init( &chan->can_be_used, NULL );

	/* release exclusive access to channels list */
	pthread_mutex_unlock( &mutex_channel_list );

	return chan;
}

/*
   return a channel (if not found, return null).
   caller function MUST lock 'mutex_channel_list' before calling this function.
 */
static channel *channel_unlocked_get( const char *chname ) {

  channel *chan;

  lua_getglobal( chanls, LUAPROC_CHANNELS_TABLE );
  lua_getfield( chanls, -1, chname );
  chan = (channel *)lua_touserdata( chanls, -1 );
  lua_pop( chanls, 2 );  /* pop userdata and channel */

  return chan;
}

/*
   return a channel (if not found, return null) with its (mutex) lock set.
   caller function should unlock channel's (mutex) lock after calling this
   function.
 */
static channel *channel_locked_get( const char *chname ) {

  channel *chan;

  /* get exclusive access to channels list */
  pthread_mutex_lock( &mutex_channel_list );

  /*
     try to get channel and lock it; if lock fails, release external
     lock ('mutex_channel_list') to try again when signaled -- this avoids
     keeping the external lock busy for too long. during the release,
     the channel may be destroyed, so it must try to get it again.
  */
  while ((( chan = channel_unlocked_get( chname )) != NULL ) &&
        ( pthread_mutex_trylock( &chan->mutex ) != 0 )) {
    pthread_cond_wait( &chan->can_be_used, &mutex_channel_list );
  }

  /* release exclusive access to channels list */
  pthread_mutex_unlock( &mutex_channel_list );

  return chan;
}

/********************************
 * exported auxiliary functions *
 ********************************/

/* unlock access to a channel and signal it can be used */
void luaproc_unlock_channel( channel *chan ) {

  /* get exclusive access to channels list */
  pthread_mutex_lock( &mutex_channel_list );
  /* release exclusive access to operate on a particular channel */
  pthread_mutex_unlock( &chan->mutex );
  /* signal that a particular channel can be used */
  pthread_cond_signal( &chan->can_be_used );
  /* release exclusive access to channels list */
  pthread_mutex_unlock( &mutex_channel_list );

}

/* insert lua process in recycle list */
void luaproc_recycle_insert( luaproc *lp ) {

  /* get exclusive access to recycled lua processes list */
  pthread_mutex_lock( &mutex_recycle_list );

  /* is recycle list full? */
  if ( list_count( &recycle_list ) >= recyclemax ) {
    /* destroy state */
    lua_close( luaproc_get_state( lp ));
  } else {
    /* insert lua process in recycle list */
    list_insert( &recycle_list, lp );
  }

  /* release exclusive access to recycled lua processes list */
  pthread_mutex_unlock( &mutex_recycle_list );
}

/* queue a lua process that tried to send a message */
void luaproc_queue_sender( luaproc *lp ) {
  list_insert( &lp->chan->send, lp );
}

/* queue a lua process that tried to receive a message */
void luaproc_queue_receiver( luaproc *lp ) {
  list_insert( &lp->chan->recv, lp );
}

/********************************
 * internal auxiliary functions *
 ********************************/
static void luaproc_loadbuffer( lua_State *parent, lua_State *lstate,
                                const char *code, size_t len ) {

	/* load lua process' lua code */
	int ret = luaL_loadbuffer( lstate, code, len, code );

	/* in case of errors, close lua_State and push error to parent */
	if ( ret != 0 ) {
		lua_pushstring( parent, lua_tostring( lstate, -1 ));
		lua_close( lstate );
		luaL_error( parent, lua_tostring( parent, -1 ));
	}
}

/* return the lua process associated with a given lua state */
static luaproc *luaproc_getself( lua_State *L ) {

  luaproc *lp;

  lua_getfield( L, LUA_REGISTRYINDEX, "LUAPROC_LP_UDATA" );
  lp = (luaproc *)lua_touserdata( L, -1 );
  lua_pop( L, 1 );

  return lp;
}

/* create new lua process */
static luaproc *luaproc_new( lua_State *L ) {

  luaproc *lp;
  lua_State *lpst = luaL_newstate();  /* create new lua state */	

  /* store the lua process in its own lua state */
  lp = (luaproc *)lua_newuserdata( lpst, sizeof( struct stluaproc ));
  lua_setfield( lpst, LUA_REGISTRYINDEX, "LUAPROC_LP_UDATA" );
  luaproc_openlualibs( lpst );  /* load standard libraries and luaproc */
  /* register luaproc's own functions */
  requiref( lpst, "luaproc", luaproc_loadlib, TRUE );
  lp->lstate = lpst;  /* insert created lua state into lua process struct */
  
  return lp;
}

/* join schedule workers (called before exiting Lua) */
static int luaproc_join_workers( lua_State *L ) {
  sched_join_workers();
  lua_close( chanls );
  return 0;
}

/* writer function for lua_dump */
static int luaproc_buff_writer( lua_State *L, const void *buff, size_t size, 
                                void *ud ) {
  (void)L;
  luaL_addlstring((luaL_Buffer *)ud, (const char *)buff, size );
  return 0;
}

/***********************
 * transfer functions *
 ***********************/

/* 
it transfer userdata between Lua states

params:

Lfrom	: sender Lua state
Lto		: receiver Lua state
i		: index within the Lfrom's stack at which the userdata is stored
type_	: kind of message transfer

return values:

TRUE in succesfully transfers; otherwise, FALSE plus error messages
*/
static int transferUdata(lua_State *Lfrom, int i, lua_State *Lto, enum t_transfer type_){
	
	//type of message transfer
	int type_transfer = 0;
	
	//stack's lenght
	int stack_len = 0;
	
	//indicates whether it is necessary to used a default transfer function (set true, by default)
	int no_transfer_function = 1;
	
	//userdata metatable name
	const char* mt_name = NULL;
	size_t str_len;

	if(type_ == to_normal)
		type_transfer = 1;
	else if(type_ == from_normal)
		type_transfer = 2;
	else if(type_ == to_temp)
		type_transfer = 3;
	else
		type_transfer = 4;
	
	//when sending messages
	if(type_ == to_normal || type_ == to_temp){
		
		//does the userdata have metatable?
		if(!lua_getmetatable(Lfrom, i)){
			
			//if not, it leaves error messages and return FALSE
			lua_pushnil(Lfrom);
			lua_pushstring(Lfrom, "userdata to be transferred as a parameter has no metatable");
			
			if(type_ == to_normal){
				lua_settop(Lto, 1);
				lua_pushnil(Lto);
				lua_pushstring(Lto, "userdata to be transferred as a parameter has no metatable");
			}
			
			return FALSE;
		}
		
		//was the userdata already transferred?
		luaL_getmetatable(Lfrom, LUAPROC_DENIED_MTUDATA);
		
		if(!lua_isnil(Lfrom, -1) && isequal(Lfrom, -1, -2)){
			
			//if so, it leaves error messages and return FALSE
			lua_pushnil(Lfrom);
			lua_pushstring(Lfrom, "trying to transfer an userdata already transferred");
			
			if(type_ == to_normal){
				lua_settop(Lto, 1);
				lua_pushnil(Lto);
				lua_pushstring(Lto, "trying to transfer an userdata already transferred");
			}

			return FALSE;
		}
		
		//otherwise, removes the blocking metatable from the stack
		lua_pop(Lfrom, 1);
		
		//getting the metatable name
		lua_pushstring(Lfrom, "__name");
		lua_rawget(Lfrom, -2);
	
		if(lua_isnil(Lfrom, -1)){
			
			//if the userdata has no a __name metafield, it leaves error messages and return FALSE
			lua_pushstring(Lfrom, "userdata has no a __name metafield");
			
			if(type_ == to_normal){
				lua_settop(Lto, 1);
				lua_pushnil(Lto);
				lua_pushstring(Lto, "userdata has no a __name metafield");
			}
			
			return FALSE;
		}
		
		//getting the lenght of the stack at this point
		stack_len = lua_gettop(Lfrom);
		
		//in the stack: ud mt | ud mt name
		
		//getting the transferable_udata table
		lua_pushlightuserdata(Lfrom, (void *)transferable_udata);
		lua_rawget(Lfrom, LUA_REGISTRYINDEX);
		
		if(!lua_isnil(Lfrom, -1)){
			//in the stack: ud mt | ud mt name | transf_udata table
		
			//getting the transfer functions associated to the userdata
			lua_pushvalue(Lfrom, -2);//in the stack: ud mt | ud mt name | transf_udata table | ud mt name
			lua_rawget(Lfrom, -2);//onto the stack: table containing send and recv functions, or a nil value (if the userdata has no associated transfer functions)
			
			if(lua_type(Lfrom, -1) == LUA_TTABLE){
				//in the stack: ud mt | ud mt name | transf_udata table | funcs_table
				
				//getting the sender transfer function
				lua_pushstring(Lfrom, "send");
				lua_rawget(Lfrom, -2);
				
				//in the stack: ud mt | ud mt name | transf_udata table | funcs_table | sender function
				
				if(lua_tocfunction(Lfrom, -1) != NULL){
					
					//indicates that there is a sender transfer function for that userdata
					no_transfer_function = 0;
					
					//inserting that function before the transf_udata table table
					lua_insert(Lfrom, -3);
					
					//in the stack: ud mt | ud mt name | sender function | transf_udata table | funcs_table 
				}
				else{
					lua_pop(Lfrom, 1);
				}
			}
			
			lua_pop(Lfrom, 1);
		}
		
		lua_pop(Lfrom, 1);
		
		if(no_transfer_function)
			//if the userdata has no transfer functions, that predefined is used
			lua_pushcfunction(Lfrom, luaproc_default_send_udata);
		
		//in the stack: ud mt | ud mt name | send transfer function
		
		//placing the parameters of the sender transfer function onto the stack
		lua_pushinteger(Lfrom, type_transfer);
		lua_pushvalue(Lfrom, i);
		lua_pushlightuserdata(Lfrom, (void *)Lto);
		
		//executing the transfer function
		if(lua_pcall(Lfrom, 3, LUA_MULTRET, 0) != 0){
			lua_pushnil(Lfrom);
			lua_insert(Lfrom, -2);
			
			if(type_ == to_normal){
				lua_settop(Lto, 1);
				lua_pushnil(Lto);
				lua_pushstring(Lto, "error while transferring an userdata from the sender Lua process");
			}
			
			return FALSE;
		}
		
		//if an error ocurrs, the sender transfer function must leave error messages in the normal Lua states involved in the transfer (because one of them may be a container Lua state)
		//if the transfer is succesfully completed, the sender transfer function must only leave the resulting userdata in the receiver Lua state
		//any value placed in the sender Lua state after calling a sender transfer function will be considered an error signal
		if(lua_gettop(Lfrom) != stack_len)
			return FALSE;
		
		//in the stack: ud mt | ud mt name
		lua_pop(Lfrom, 2);
		
		//this metatable is associated to the original userdata, in order to avoid method calls on this (also indicates that a userdata have already been transferred)
		luaL_getmetatable(Lfrom, LUAPROC_DENIED_MTUDATA);
		
		if(lua_isnil(Lfrom, -1)){
			
			lua_pop(Lfrom, 1);
			
			luaL_newmetatable(Lfrom, LUAPROC_DENIED_MTUDATA);
			
			lua_pushstring(Lfrom, "__index");	
			lua_pushcfunction(Lfrom, luaproc_denied_udata);
			lua_rawset(Lfrom, -3);
			
			lua_pushstring(Lfrom, "__metatable");
			lua_pushstring(Lfrom, "Access denied");
			lua_rawset(Lfrom, -3);		
		}
		
		//associating the above metatable to the original userdata
		lua_setmetatable(Lfrom, i);
		
	}
	else{//when receiving messages, "type_" may be either "from_normal" or "from_temp" 
		
		//does the userdata have metatable?
		if(!lua_getmetatable(Lfrom, i)){
			
			//if not, it leaves error messages and return FALSE
			if(type_ == from_normal){
				lua_pushnil(Lfrom);
				lua_pushstring(Lfrom, "userdata to be transferred as a parameter has no metatable");
			}
			
			lua_settop(Lto, 1);
			lua_pushnil(Lto);
			lua_pushstring(Lto, "userdata to be transferred has no metatable");
			
			return FALSE;
		}
		
		//a message received from a container Lua state is immediately
		//so we have to ckecks whether an userdata was already transferred just when receiving from a normal Lua state
		if(type_ == from_normal){
			//was the userdata already transferred?
			luaL_getmetatable(Lfrom, LUAPROC_DENIED_MTUDATA);
			
			if(!lua_isnil(Lfrom, -1) && isequal(Lfrom, -1, -2)){
				
				//if so, it leaves error messages and return FALSE
				lua_pushnil(Lfrom);
				lua_pushstring(Lfrom, "trying to transfer an userdata already transferred");
				
				lua_settop(Lto, 1);
				lua_pushnil(Lto);
				lua_pushstring(Lto, "trying to transfer an userdata already transferred");

				return FALSE;
			}
			
			//otherwise, removes the blocking metatable from the stack
			lua_pop(Lfrom, 1);
		}
		
		//Lfrom's stack: ud mt
		
		lua_pushstring(Lfrom, "__name");
		lua_rawget(Lfrom, -2);
		
		if(lua_isnil(Lfrom, -1)){
			
			//if the userdata has no a __name metafield, it leaves error messages and return FALSE
			if(type_ == from_normal){
				lua_pushstring(Lfrom, "userdata does not have a __name metafield");
			}
			
			lua_settop(Lto, 1);
			lua_pushnil(Lto);
			lua_pushstring(Lto, "userdata does not have a __name metafield");
			
			return FALSE;
		}
		
		//Lfrom's stack: ud mt | ud mt name
		
		mt_name = lua_tolstring(Lfrom, -1, &str_len);
		
		lua_pushlstring(Lto, mt_name, str_len);
		
		//Lto's stack: ud mt name
		
		stack_len = lua_gettop(Lto);
		
		lua_pushlightuserdata(Lto, (void *)transferable_udata);
		lua_rawget(Lto, LUA_REGISTRYINDEX);
		
		if(!lua_isnil(Lto, -1)){
			
			//Lto's stack: ud mt name | transf_udata table
			
			//getting the transfer functions associated to the userdata
			lua_pushvalue(Lto, -2);//Lto's stack: ud mt name | transf_udata table | ud mt name
			lua_rawget(Lto, -2);//onto the stack: table containing send and recv functions, or a nil value (if the userdata has no associated transfer functions)
			
			if(lua_type(Lto, -1) == LUA_TTABLE){
				//Lto's stack: ud mt name | transf_udata table | funcs_table
				
				//getting the receiver transfer function
				lua_pushstring(Lto, "recv");
				lua_rawget(Lto, -2);
				
				//Lto's stack: ud mt name | transf_udata table | funcs_table | receiver transfer function
				
				if(lua_tocfunction(Lto, -1) != NULL){
					
					//indicates that there is a receiver transfer function for that userdata
					no_transfer_function = 0;
					
					//inserting that function before the transf_udata table table
					lua_insert(Lto, -3);
					
					//Lto's stack: ud mt name | receiver transfer function | transf_udata table | funcs_table 
				}
				else{
					lua_pop(Lto, 1);
				}
			}
			
			lua_pop(Lto, 1);
		}
		
		lua_pop(Lto, 1);
		
		if(no_transfer_function)
			//if the userdata has no transfer functions, that predefined is used
			lua_pushcfunction(Lto, luaproc_default_recv_udata);
		
		//Lto's stack: ud mt name | receiver transfer function 
		
		//placing the parameters of the recv transfer function
		lua_pushinteger(Lto, type_transfer);
		lua_pushinteger(Lto, i);
		lua_pushlightuserdata(Lto, (void *)Lfrom);
		
		//executing the transfer function
		if(lua_pcall(Lto, 3, LUA_MULTRET, 0) != 0){
			
			if(type_ == from_normal){
				lua_pushnil(Lfrom);
				lua_pushstring(Lfrom, "error while transferring an userdata");
			}
			
			lua_settop(Lto, 1);
			lua_pushnil(Lto);
			lua_pushstring(Lto, "error while transferring an userdata");
			
			return FALSE;
		}
		
		//Lto's stack: ud mt name | resulting userdata
		
		//the receiver transfer function must leave nothing but the resulting userdata
		if(lua_touserdata(Lto, -1) == NULL || lua_gettop(Lto) != stack_len + 1)
			return FALSE;
		
		//removing ud mt name 
		lua_remove(Lto, -2);
		
		//Lto's stack: resulting userdata
		
		//removing the remaining two values in the Lfrom's stack: ud mt | ud mt name 
		lua_pop(Lfrom, 2);
		
		if(type_ == from_normal){
			
			//it only inhabilites userdata transferred from normal Lua state, because they dont have their metatables in container Lua states 
			luaL_getmetatable(Lfrom, LUAPROC_DENIED_MTUDATA);
		
			if(lua_isnil(Lfrom, -1) == 1){
				
				lua_pop(Lfrom, 1);
				
				luaL_newmetatable(Lfrom, LUAPROC_DENIED_MTUDATA);
				
				lua_pushstring(Lfrom, "__index");	
				lua_pushcfunction(Lfrom, luaproc_denied_udata);
				lua_rawset(Lfrom, -3);
				
				lua_pushstring(Lfrom, "__metatable");
				lua_pushstring(Lfrom, "Access denied");
				lua_rawset(Lfrom, -3);		
			}
			
			//associates the the above metatable to the original userdata
			lua_setmetatable(Lfrom, i);
		}
	}
	
	return TRUE;
}

/* 
it transfer tables between Lua states

params:

Lfrom	: sender Lua state
Lto		: receiver Lua state
index	: index within the Lfrom's stack at which the userdata is stored
type_	: kind of message transfer
lv:		: deep level in recursive calls

return values:

TRUE in succesfully transfers; otherwise, FALSE plus error messages
*/

static int transferTable(lua_State *Lfrom, int index, lua_State *Lto, enum t_transfer type_, int lv){
	
	size_t len;
	
	//stores string values
	const char *str;
	
	//ndex within the Lfrom's stack at which the key of an entry is stored
	int idxKey = 0;
	
	//ndex within the Lfrom's stack at which the value of an entry is stored
	int idxValue = 0; 
	
	int ret = 0;
	
	//checks whether the number of nesting levels is allowed
	if(lv > MAX_NESTING_LEVELS){
		if(type_ != to_temp){
			lua_settop( Lto, 1 );
			lua_pushnil( Lto );
			lua_pushstring( Lto, "number of nesting levels not supported");
		}
		
		if(type_ != from_temp){
			lua_pushnil( Lfrom );
			lua_pushstring( Lfrom, "number of nesting levels not supported");
		}
		
		return FALSE;
	}
	
	//checks whether a table tranferred from a container Lua state represents a C function
	if(lv == 1 && type_ == from_temp && (ret = transferCFunction(Lfrom, index, Lto, type_)) < 1){
	
		//if the table represents a C function and was not transferred successfully, it returns FALSE
		if(ret == -1)
			return FALSE;
		
		return TRUE;
	}
	
	//creates an equivalent table in the receiver Lua state
	lua_newtable(Lto);	
	
	//travels the table to be transferred
	lua_pushnil(Lfrom);
	while (lua_next(Lfrom, index) != 0)
	{
		//transfers the key
		
		idxKey = lua_gettop(Lfrom) - 1;
		
		switch (lua_type( Lfrom, -2 )) {
			case LUA_TNUMBER:
				 lua_pushnumber(Lto, lua_tonumber(Lfrom, -2 ));
				break;
			case LUA_TSTRING:
				 str = lua_tolstring(Lfrom, -2, &len);
				 lua_pushlstring(Lto, str, len);
				break;			
			case LUA_TBOOLEAN:
				lua_pushboolean(Lto, lua_toboolean(Lfrom, -2));
				break;
			case LUA_TTABLE:
				if(!transferTable(Lfrom, idxKey, Lto, type_, lv + 1))
					return FALSE;
				break;
			case LUA_TUSERDATA:
				if(!transferUdata(Lfrom, idxKey, Lto, type_))
					return FALSE;
				break;
			case LUA_TFUNCTION:
				if(!transferFunction(Lfrom, idxKey, Lto, type_))
					return FALSE;
				break;
			default://leaves error messages when attempting to transfer types not supported (threads, corroutines)
				
				if(type_ != to_temp){
					lua_settop( Lto, 1 );
					lua_pushnil( Lto );
					lua_pushfstring( Lto, "failed to receive key of unsupported type '%s'", luaL_typename( Lfrom, idxKey ));
				}
				
				if(type_ != from_temp){
					lua_pushnil( Lfrom );
					lua_pushfstring( Lfrom, "failed to send key of unsupported type '%s'", luaL_typename( Lfrom, idxKey));
				}
				
				return FALSE;
		}
		
		//transfers the value
		
		idxValue = lua_gettop(Lfrom);
		
		switch ( lua_type( Lfrom, -1 )) {
			case LUA_TNUMBER:
				 lua_pushnumber(Lto, lua_tonumber(Lfrom, -1 ));
				 break;
			case LUA_TSTRING:
				str = lua_tolstring(Lfrom, -1, &len);
				lua_pushlstring(Lto, str, len);
				break;			
			case LUA_TBOOLEAN:
				lua_pushboolean(Lto, lua_toboolean(Lfrom, -1));
				break;
			case LUA_TNIL:
				lua_pushnil(Lto);
				break;
			case LUA_TTABLE:
				if(!transferTable(Lfrom, idxValue, Lto, type_, lv + 1))
					return FALSE;
				break;
			case LUA_TUSERDATA:
				if(!transferUdata(Lfrom, idxValue, Lto, type_))
					return FALSE;
				break;
			case LUA_TFUNCTION:
				if(!transferFunction(Lfrom, idxValue, Lto, type_))
					return FALSE;
				break;
			default://leaves error messages when attempting to transfer types not supported (threads, corroutines) and returns FALSE
				
				if(type_ != to_temp){
					lua_settop( Lto, 1 );
					lua_pushnil( Lto );
					lua_pushfstring( Lto, "failed to receive value of unsupported type '%s'", luaL_typename( Lfrom, idxValue));
				}
				
				if(type_ != from_temp){
					lua_pushnil( Lfrom );
					lua_pushfstring( Lfrom, "failed to send value of unsupported type '%s'", luaL_typename( Lfrom, idxValue));
				}
				
				return FALSE;
		}
		
		//inserts the copied entry into the equivalent table
		lua_rawset(Lto, -3);
		
		//removes the value to proceed with the next iteration
		lua_pop(Lfrom, 1);
	}
	
	return TRUE;
}

/* 
it checks whether the table stored by the upvalue onto the stack is the table registered by a C module.
If so, it attemps to tranfer this tables. Otherwise, it notifies that this is not table registered by a C module

params:

Lfrom	: sender Lua state
Lto		: receiver Lua state
type_	: kind of message transfer

return values:

0		: the table is not a table registered by a C module
-1		: the equivalent table is not registered in the receiver Lua state
1		: the equivalent table was located and pushed onto the receiver's stack as a result of the transfer

*/
static int copymodule(lua_State *Lfrom, lua_State *Lto, enum t_transfer type_){
	
	//return value
	int result = 0;
	
	//indicates whether the table onto the sender's stack is a table registered by a C module (0: FALSE, 1: TRUE)
	int forward = 0;
	
	//stores the name used by the C module for registering this table
	const char *tableName;
	size_t str_len;
	
	//receiving from a container Lua state
	if(type_ == from_temp){
		
		//in container Lua states, a table registered by a C module is represent by a table with a specific metatable
		//this last table stores the name used by the C module for registering that table it represents 
		
		//getting the table metatable
		if(!lua_getmetatable(Lfrom, -1))
			return result;
		
		//getting the metatable that indicates a table is representing a table registered by a C module
		luaL_getmetatable(Lfrom, MT_MODULE);
		
		//checks whether both metatables are equal
		if(!lua_isnil(Lfrom, -1) && isequal( Lfrom, -1, -2 )){
			
			//If so, it indicates the table to be transferred is representing a table registered by a C module
			forward = 1;
			
			//getting the name used by the C module for registering the represented table
			lua_pushstring(Lfrom, "__name");
			lua_rawget(Lfrom, -4);
			
			tableName = lua_tolstring(Lfrom, -1, &str_len);
			lua_pop(Lfrom, 1);
		}
		
		//removes the two metatables from the stack
		lua_pop(Lfrom, 2);
	}
	else{
		//sending/receiving from a normal Lua state
		
		//looking for the table onto the sender's stack in the "package.loaded" table
		lua_getglobal(Lfrom, "package" );
		if ( lua_type( Lfrom, -1 ) == LUA_TTABLE ) {
			lua_getfield( Lfrom, -1, "loaded" );
			if ( lua_type( Lfrom, -1 ) == LUA_TTABLE ) {
				
				int stack_len = lua_gettop(Lfrom);
				
				lua_pushnil(Lfrom);
				while(lua_next(Lfrom, stack_len) != 0){
					
					//compares the table to be transferred with that obtained in a specific iteration
					if(isequal(Lfrom, stack_len - 2, -1)){
						
						//if they are equal, it gets the name used by the C module for registering this table
						tableName = lua_tolstring(Lfrom, -2, &str_len);
						
						//indicates that the table to be transferred is a table registered by a C module
						forward = 1;
					}
					
					lua_pop(Lfrom, 1);
				}
			}
			lua_pop(Lfrom, 1);
		}
		lua_pop(Lfrom, 1);
	}
	
	
	if (forward) {
		if(type_ != to_temp){
			
			//if the table to be transferred is a table registered by a C module and it is not being transferred to a container Lua state
			//it looks for the equivalent function in the "package.loaded" table of the receiver Lua state
			lua_getglobal(Lto, "package" );
			if ( lua_type( Lto, -1 ) == LUA_TTABLE ) {
				lua_getfield( Lto, -1, "loaded" );
				if ( lua_type( Lto, -1 ) == LUA_TTABLE ) {
					lua_getfield( Lto, -1, tableName);
					if ( lua_type( Lto, -1 ) == LUA_TTABLE ) {
						
						//indicates the equivalent table is registered in the receiver Lua state, and inserts this table before the "package" table
						result = 1;
						lua_insert(Lto, -3);
					}
					else{
						//indicates that there is no an equivalent table in the receiver Lua state
						result = -1;
						lua_pop(Lto, 1);
					}
				}
				lua_pop(Lto, 1);
			}
			lua_pop(Lto, 1);
		}
		else{
			
			//if the table to be transferred is a table registered by a C module and it is being transferred to a container Lua state
			//it creates a table and stores in this, the name the name used by the C module for registering the table in the sender Lua state
			result = 1;
			
			lua_newtable(Lto);
			lua_pushstring(Lto, "__name");
			lua_pushlstring(Lto, tableName, str_len);
			lua_rawset(Lto, -3);
			
			//indicates that the table created represents a table registered by a C module
			luaL_newmetatable(Lto, MT_MODULE);
			lua_setmetatable(Lto, -2);
		}
	}
	
	
	if (result == -1){
		
		//if there is no an equivalent table in the receiver Lua state, it leaves error messages in the Lua state involved in the transfer
		if(type_ != from_temp){
			lua_pushnil(Lfrom);
			lua_pushfstring(Lfrom, "- %s - module must be loaded in the receiver Lua process", tableName);
		}
		
		lua_settop(Lto, 1);
		lua_pushnil(Lto);
		lua_pushfstring( Lto, "- %s - module must be loaded in this Lua process", tableName);
	}
	
	return result;
}


/* 
It travels recursively a table, looking for a given C function.
If it finds this function, it stores in "pathTable" table the path leading to this function

params:

Lfrom		: sender Lua state
visitedTable	: index within the sender Lua state at which the table storing the tables already visited is stored
indexTable	: index within the sender Lua state at which the table to be transferred is stored
funcIndex	: index within the sender Lua state at which the C function to be found is stored
pathTable		: index within the sender Lua state at which the table storing the path to the C function is stored
lv			: deep level of the recursive call
type_		: kind of message transfer

return values:

0		: it did not find the given C function
1		: otherwise

*/

static int travel_table(lua_State *Lfrom, int visitedTable, int indexTable, int funcIndex, int pathTable, int lv, enum t_transfer type_){

	//return value
	int result = 0;
	
	//stores an entry's key
	const char *str = NULL;

	//stores the key of the entry storing the environment
	const char *pack = "_G";
	size_t len;
	
	//marks the table to be traveled as visited
	lua_pushvalue(Lfrom, indexTable);
	lua_pushboolean(Lfrom, TRUE);
	lua_rawset(Lfrom, visitedTable);
	
	//traveling the table
	lua_pushnil(Lfrom);
	while(lua_next(Lfrom, indexTable) != 0){
		
		if(lua_type( Lfrom, -2 ) == LUA_TSTRING && strcmp(str = lua_tolstring(Lfrom, -2, &len), pack) != 0){
			
			//if an entry has a string as a key and it does not store the environment (this method does not look for the C function through the environment table)
			switch ( lua_type( Lfrom, -1 )) {
				case LUA_TTABLE:
					
					//checks whether the table stored in an entry have already been visited
					lua_pushvalue(Lfrom, -1);
					lua_rawget(Lfrom, visitedTable);
				
					if(lua_isnil(Lfrom, -1)){
						
						//If not, it proceeds to travel this table
						lua_pop(Lfrom, 1);
						result = travel_table(Lfrom, visitedTable, lua_gettop(Lfrom), funcIndex, pathTable, lv + 1, type_);
					}
					else{
						lua_pop(Lfrom, 1);
					}
					
					break;
				case LUA_TFUNCTION:
					
					//checks whether this entry stores the equivalent C function we are looking for
					if(lua_tocfunction(Lfrom, funcIndex) == lua_tocfunction(Lfrom, -1))
						result = 1;
					
					break;
			}
		}
		
		if(result == 1){
			
			//if the equivalent C function was found through this entry, it adds this entry's key to the path leading to the C function
			lua_pushinteger(Lfrom, lv);
			lua_pushvalue(Lfrom, -3);
			lua_rawset(Lfrom, pathTable);
			
			//removing both key and entry's value
			lua_pop(Lfrom, 2);
			
			//notifies that the given C function was found through this entry
			return 1;
		}
		
		//removes the entry's value
		lua_pop(Lfrom, 1);
	}
	
	//indicates that the given C function was not found through the traveled table
	return 0;
}

/* 
it transfers C functions between Lua states

params:

Lfrom	: sender Lua state
Lto		: receiver Lua state
index	: index within the Lfrom's stack at which the C function is stored
type_	: kind of message transfer

return values:

0		: sucessfull copy, as the C function is registered in both sender and receiver Lua state
-1		: the equivalent table is not registered in the receiver Lua state
1		: if the C function is not registered in the receiver Lua state

*/

static int transferCFunction(lua_State *Lfrom, int index, lua_State *Lto, enum t_transfer type_){
	
	const char *str = NULL;
	size_t len;
	
	//number of tables traveled in the receiver Lua state for reaching the equivalent C function
	int num_elem = 0;

	//indicates whether the C function to be transferred is found in the sender Lua state
	//in case of receiving from a container Lua state, it indicates whether the table represents a C function
	int found = 1;
	
	//return value
	int ret = 0;

	//receiving a function from a container Lua state
	if(type_ == from_temp){
		
		//in container Lua states, C functions are represented by specific tables, so we must check if this is the case 
		
		//getting the metatable associated to the table onto the container Lua state's stack
		if(!lua_getmetatable(Lfrom, -1))
			return 1;
		
		//getting the metatable indicating that a table represents a C function
		luaL_getmetatable(Lfrom, MT_CFUNCTION);
		
		if(!lua_isnil(Lfrom, -1) && isequal( Lfrom, -1, -2 ))
			//if both metatables are equal, this table represents a C function
			found = 1;
		else
			found = 0;
			
		//removes both metatables from the container Lua state's stack
		lua_pop(Lfrom, 2);
	}
	else{
	
		//when searching for the C function in the "package.loaded" table, this table is used to mark those tables already visited, thus avoiding infinite recursive cycles
		lua_newtable(Lfrom);
		
		//getting the table storing C functions transferred from Lfrom
		lua_pushlightuserdata(Lfrom, (void *)func_path);
		lua_rawget(Lfrom, LUA_REGISTRYINDEX);
		
		if(lua_isnil(Lfrom, -1)){
			lua_pop(Lfrom, 1);
			
			//creating the "func_path" table in Lfrom, if it not exist
			lua_newtable(Lfrom);
			lua_pushlightuserdata(Lfrom, (void *)func_path);
			lua_pushvalue(Lfrom, -2);
			lua_rawset(Lfrom, LUA_REGISTRYINDEX);
		}
		
		//"func_path" table onto the stack
		
		//getting the path associated to the C function to be transferred, from the "func_path" table
		//as a result we must obtain a table storing in each entry a step in the path leading to the C function to be tranferred 
		lua_pushlightuserdata(Lfrom, (void *)lua_tocfunction(Lfrom, index));
		lua_rawget(Lfrom, -2);
		
		if(lua_isnil(Lfrom, -1)){
			lua_pop(Lfrom, 1);
			
			//if the C function is being tranferred for the first time, we must create and fill in a "path_table" table storing path leading to the C function
			lua_newtable(Lfrom);
			
			lua_getglobal(Lfrom, "package" );
			if ( lua_type( Lfrom, -1 ) == LUA_TTABLE ) {
				
				//marking the "package" table as a visited table
				lua_pushvalue(Lfrom, -1);
				lua_pushboolean(Lfrom, TRUE);
				lua_rawset(Lfrom, -6);
				
				lua_getfield( Lfrom, -1, "loaded" );
				if ( lua_type( Lfrom, -1 ) == LUA_TTABLE ) {
					
					//looking for the C function among those functions registered in the "package.loaded" table
					found = travel_table(Lfrom, lua_gettop(Lfrom) - 4, lua_gettop(Lfrom), index, lua_gettop(Lfrom) - 2,  1, type_);
				}
				lua_pop(Lfrom, 1);
			}
			lua_pop(Lfrom, 1);
			
			if(found == 1){
				
				//if the function was found, it is stored in the "func_path" table
				lua_pushlightuserdata(Lfrom, (void *)lua_tocfunction(Lfrom, index));
				lua_pushvalue(Lfrom, -2);
				lua_rawset(Lfrom, -4);
			}
		}
	}
	
	if(found == 1){
		
		//at this point we have the "path_table" table onto the sender Lua state's stack
		
		if(type_ != to_temp){
			
			//if the receiver is not a container Lua state, the mechanism looks for the equivalent C function in its "package.loaded" table
			lua_getglobal(Lto, "package" );
			if ( lua_type( Lto, -1 ) == LUA_TTABLE ) {
				lua_getfield( Lto, -1, "loaded" );
				if ( lua_type( Lto, -1 ) == LUA_TTABLE ) {
					
					int whileFinished = 0, noTable = 0;
					
					while(TRUE){
						
						//looking for the equivalent C function (in the receiver Lua state) by following the path stored in the "path_table"
						lua_pushinteger(Lfrom, num_elem + 1);
						lua_rawget(Lfrom, -2);
						
						//we are reached the end of the path
						if(lua_isnil(Lfrom, -1)){
							whileFinished = 1;
							break;
						}	
						
						//we haven't reached the end of the path and we cannot index the value onto the receiver's stack, as it is not a table
						if(noTable)
							break;
						
						//increasing the number of elements pushed in the receiver's stack
						num_elem++;
				
						//getting a chunk of the path
						str = lua_tolstring(Lfrom, -1, &len);
						
						//looking for an entry indexed by the obtained chunk of path, in the table onto the receiver's stack
						lua_pushlstring(Lto, str, len);
						lua_rawget(Lto, -2);
						
						//if the obtained element is not a table, we cannot index it in next round
						if(lua_type( Lto, -1 ) != LUA_TTABLE)
							noTable = 1;
						
						//removing the chunk of path from the sender Lua state
						lua_pop(Lfrom, 1);
					}
					
					//removing the value onto the sender's stack (it may be a nil value or a chunk of the path)
					lua_pop(Lfrom, 1);
					
					//if the previous "while" was not interrupted, we check whether the element onto the receiver's stack is a C function 
					if(whileFinished == 1 && lua_tocfunction(Lto, -1) != NULL){
						
						//inserting the C function before the "package" table
						lua_insert(Lto, -1*(num_elem + 2));
						num_elem--;
					}
					else{
						//it couldn't found the equivalent C function by following the path indicated by the "path_table" table
						ret = -1;
					}
					
					//removing all the tables through which we attempt to reach the equivalent C function
					lua_pop(Lto, num_elem);
				}
				lua_pop(Lto, 1);
			}
			lua_pop(Lto, 1);
		}
		else{
		
			//if the receiver Lua state is a container Lua state, there is no an equivalent C function in this, 
			//so the mechanism copies to it the "path_table" table and marks this table as a table representing a C function
			transferTable(Lfrom, lua_gettop(Lfrom), Lto, type_, 1);
			
			luaL_newmetatable(Lto, MT_CFUNCTION);
			lua_setmetatable(Lto, -2);
		}
	}
	else{
		//indicates either the table onto the sender's stack (in case of container Lua states) does not represent a C function
		//or the C function to be transferred was not found in the sender Lua state
		ret = 1;
	}
	
	//if the C function is being transferred from a normal Lua state, we still have 3 elements onto the sender's stack
	if(type_ != from_temp)
		lua_pop(Lfrom, 3);
	
	if (ret == -1){
		
		//we only get to this point if the C function is not registered in the receiver Lua state (a normal Lua state), but it did was found in the sender Lua state
		
		if(type_ != from_temp){
			lua_pushnil(Lfrom);
			lua_pushstring(Lfrom, "C function must be registered in the receiver Lua process");
		}
		
		lua_settop(Lto, 1);
		lua_pushnil(Lto);
		lua_pushstring( Lto, "C function must be registered in the receiver Lua process");
	}
	
	return ret;
}


/* 
it copies upvalues between lua states' stacks and sets them as upvalues of the function onto the receiver Lua stack

params:

Lfrom	: sender Lua state
Lto		: receiver Lua state
funcindex	: index within the Lfrom's stack at which the Lua function to be transferred is stored
type_	: kind of message transfer

return values:

TRUE	: if all the upvalues of the Lua function were transferred and settled as upvalues of the resulting function sucessfully
FALSE	: otherwise

*/

static int luaproc_copyupvalues( lua_State *Lfrom, lua_State *Lto, int funcindex, enum t_transfer type_) {

	int i = 1;
	
	//index within the Lto's stack at which the equivalent function is stored
	int top = lua_gettop(Lto);
	
	int ret = 0;
	const char *str;
	size_t len;
			 
	//checks the type of each upvalue and, if it's supported, copy it
	while ( lua_getupvalue( Lfrom, funcindex, i ) != NULL ) {
		switch ( lua_type( Lfrom, -1 )) {
			case LUA_TBOOLEAN:
				lua_pushboolean( Lto, lua_toboolean( Lfrom, -1 ));
				break;
			case LUA_TNUMBER:
				copynumber( Lto, Lfrom, -1 );
				break;
			case LUA_TSTRING: {
				str = lua_tolstring( Lfrom, -1, &len );
				lua_pushlstring( Lto, str, len );
				break;
			}
			case LUA_TNIL:
				lua_pushnil( Lto );
				break;

			case LUA_TTABLE:
				lua_pushglobaltable( Lfrom );
			
				//checks whether the upvalue is the global environment.
				if ( isequal( Lfrom, -1, -2 )) {
					
					//If so, it takes the global environment of the receiver Lua state as a result of the transfer
					lua_pop( Lfrom, 1 );
					lua_pushglobaltable( Lto );
					break;
				}
				else{
					
					//otherwise, it removes the global environment from the sender's stack
					lua_pop( Lfrom, 1 );
				}
				
				//checks whether the upvalue stores a table registered by a C module.
				if((ret = copymodule(Lfrom, Lto, type_))){
					if (ret == -1)
						//If so, but the equivalent table is not registered in the receiver Lua state, it returns FALSE
						return FALSE;
					else
						//at this point the mechanism already transferred this table
						break;
				}
				//in container Lua states, it checks whether the upvalue stores a table representing a C function.
				else if(type_ == from_temp && (ret = transferCFunction(Lfrom, lua_gettop(Lfrom), Lto, type_)) < 1){
						//If so, but the equivalent C function is not registered in the receiver Lua state, it returns FALSE
						if(ret == -1)
							return FALSE;
						else
							//at this point the mechanism already placed the equivalent C function onto the receiver's stack as a result of the transfer
							break;
					}
				else if(transferTable(Lfrom, lua_gettop(Lfrom), Lto, type_, 1))//attempts to transfer the table as a normal table
					break;
				else
					return FALSE;
			
			case LUA_TUSERDATA:
				if(!transferUdata(Lfrom, lua_gettop(Lfrom), Lto, type_))
					return FALSE;
				break;
			
			case LUA_TFUNCTION:
				if(!transferFunction(Lfrom, lua_gettop(Lfrom), Lto, type_))
					return FALSE;
				break;
				
			default: //leaves error messages when attempting to transfer types not supported (threads, corroutines)
				if(type_ != from_temp){
					lua_pushnil( Lfrom );
					lua_pushfstring( Lfrom, "failed to copy upvalue storing an unsupported type '%s'", luaL_typename( Lfrom, -2 ));
				}
				
				if(type_ != to_temp){
					lua_settop(Lto, 1);
					lua_pushnil(Lto);
					lua_pushfstring( Lto, "failed to copy upvalue storing an unsupported type '%s'", luaL_typename( Lfrom, -3 ));
				}
				
				return FALSE;
		}

		//removing the transferred upvalue from the Lfrom's stack
		lua_pop( Lfrom, 1 );
		
		//setting the transferred upvalue as an upvalue of the resulting function
		if ( lua_setupvalue( Lto, top, i ) == NULL ) {
			
			//leaving error messages depending on the transfer's type 			
			if(type_ != from_temp){
				lua_pushnil( Lfrom );
				lua_pushstring( Lfrom, "failed to set upvalues" );
			}
			
			if(type_ != to_temp){
				lua_settop(Lto, 1);
				lua_pushnil(Lto);
				lua_pushstring(Lto, "failed to set upvalues");
			}
			
			//returns FALSE if the transferred upvalue couldn't be placed as an upvalue of the resulting function
			return FALSE;
		}

		i++;
	}
	
	return TRUE;
}

/* 
it transfers Lua or C functions between Lua states

params:

Lfrom	: sender Lua state
Lto		: receiver Lua state
index	: index within the Lfrom's stack at which the function to be transferred is stored
type_	: kind of message transfer

return values:

1		: if the function was transferred sucessfully
0		: otherwise

*/

static int transferFunction(lua_State *Lfrom, int index, lua_State *Lto, enum t_transfer type_){
	
	size_t len;
	
	//buffer used for storing the binary code of a Lua function
	luaL_Buffer buff;
	
	//binary code of a Lua function
	const char *code;
	
	//indicates if the Lua function was serialized successfully
	int succs = 0;
	
	//holds the deep level in recursive calls
	int recursive_level = 0;
	
	//indicates whether C functions or upvalues were transferred successfully
	int result = 0;
	
	//it checks wheter the function to be sent is a C function
	if(lua_tocfunction(Lfrom, index)){
			
		//attempts to transfer the C function
		result = transferCFunction(Lfrom, index, Lto, type_);
		
		if(result == 0)
			//the C function was transferred successfully
			return TRUE;
		else if(result == 1){
			
			//the C function was not found in the sender Lua state
			char *str = "sender";
			
			lua_pushnil(Lfrom);
			lua_pushfstring(Lfrom, "error when attempting to transfer a C function not found in the %s Lua process", str);
			
			//the mechanism does not leave error messages in the container Lua state
			if(type_ != to_temp){
				lua_settop(Lto, 1);
				lua_pushnil(Lto);
				lua_pushfstring(Lto, "error when attempting to transfer a C function not found in the %s Lua process", str);
			}
		}
		
		return FALSE;
	}
	
	//at this point the function to be transferred is a Lua function
	
	//initilizes the buffer storing the binary code associated to the Lua function to be transferred
	luaL_buffinit( Lfrom, &buff );
	
	//pushes the Lua function onto the sender Lua's stack
	lua_pushvalue(Lfrom, index);
	
	//getting the binary code associated to the function
	succs = dump(Lfrom, luaproc_buff_writer, &buff, FALSE);
	if ( succs != 0 ) {
		
		//if it was not possible to do so, this method leaves error messages in the Lua states involved in the transfer
		if(type_ != from_temp){
			lua_pushnil( Lfrom );
			lua_pushfstring( Lfrom, "error %d dumping function to binary string, it may not be a Lua function.", succs);
		}
		
		if(type_ != to_temp){
			lua_settop(Lto, 1);
			lua_pushnil(Lto);
			lua_pushfstring( Lto, "error %d dumping function to binary string, it may not be a Lua function.", succs);
		}
		
		return FALSE;
	}
	
	//pushes the function's  binary code onto the sender Lua's stack
	luaL_pushresult(&buff);
	
	//gets (or creates) a counter to hold the times this method has been called recursively for transferring this function
	lua_pushlightuserdata(Lto, (void *)recursive_counter);
	lua_rawget(Lto, LUA_REGISTRYINDEX);
	
	if(lua_isnil(Lto, -1)){
		lua_pop(Lto, 1);
		
		//if this counter does not exist, it means that this is the first time this method is called for transferring the function
		//so the method must create a "func_cache" table for storing those Lua function that were transferred when transferring this function
		lua_newtable(Lto);
		lua_pushlightuserdata(Lto, (void *)func_cache);
		lua_pushvalue(Lto, -2);
		lua_rawset(Lto, LUA_REGISTRYINDEX);
	}
	else{
		//if the counter already existed, the "func_cache" table also existed, so we get it together with the counter
		recursive_level = lua_tointeger(Lto, -1);
		lua_pop(Lto, 1);
		
		lua_pushlightuserdata(Lto, (void *)func_cache);
		lua_rawget(Lto, LUA_REGISTRYINDEX);
	}
	
	//using a pointer to the function to be transferred for checking if this was already transferred
	lua_pushlightuserdata(Lto, (void *)lua_topointer(Lfrom, -2));
	lua_rawget(Lto, -2);
	
	if(!lua_isnil(Lto, -1)){
		
		//if this function was already transferred to the receiver Lua state, the method removes the "func_table" table and leave this function onto the receiver's stack as a result of the transfer
		lua_remove(Lto, -2);
		
		//removes the function to be transferred and its binary code from the sender's stack
		lua_pop( Lfrom, 2 );
		
		return TRUE;
	}
	
	//removes the nil value fom the receiver's stack, as this is the first time this function is transferred. The func_cache still is onto the receiver's stack
	lua_pop(Lto, 1);
	
	//getting the binary code of the function to be transferred from the sender's stack
	code = lua_tolstring(Lfrom, lua_gettop(Lfrom), &len);

	//pushes an equivalent function onto the receiver Lua's stack
	luaproc_loadbuffer(Lfrom, Lto, code, len);

	//registers the transferred function in the "func_cache" table in the receiver Lua state 
	lua_pushlightuserdata(Lto, (void *)lua_topointer(Lfrom, -2));
	lua_pushvalue(Lto, -2);
	lua_rawset(Lto, -4);
	
	//removes the "func_cache" table from the receiver's stack
	lua_remove(Lto, -2);
	
	//increasing by one the number of levels in the recursive calls performed for transferring this function, as the method is going to transfer its upvalues
	//some of these upvalues may call this method again
	lua_pushlightuserdata(Lto, (void *)recursive_counter);
	lua_pushinteger(Lto, recursive_level + 1);
	lua_rawset(Lto, LUA_REGISTRYINDEX);
	
	//removes the function to be transferred and its binary code from the sender's stack
	lua_pop( Lfrom, 2 );
	
	//attempts to transfer the upvalues of the function to be transferred
	result = luaproc_copyupvalues( Lfrom, Lto, index, type_);
	
	//after transferring these upvalues, the method decreases by one the number of levels in the recursive calls performed for transferring this function
	lua_pushlightuserdata(Lto, (void *)recursive_counter);
	lua_pushinteger(Lto, recursive_level);
	lua_rawset(Lto, LUA_REGISTRYINDEX);
	
	if(recursive_level == 0){
		
		//after transferring the function, the method resets the "func_table" table to keep an equivalence between the function to be transferred and that resulting
		lua_pushlightuserdata(Lto, (void *)func_cache);
		lua_newtable(Lto);
		lua_rawset(Lto, LUA_REGISTRYINDEX);
	}
	
	return result;
}

/* 
it transfers one value between Lua states

params:

Lfrom	: sender Lua state
Lto		: receiver Lua state
i		: index within the Lfrom's stack at which the value to be transferred is stored
type_	: kind of message transfer

return values:

TRUE	: value was transferred sucessfully
FALSE	: otherwise

*/

static int copy_one_value(lua_State *Lfrom, int i, lua_State *Lto, enum t_transfer type_){

	//return value
	int result = TRUE;
	
	const char *str;
	size_t len;
	
	//"type_" may be either "to_temp" or "from_temp"
	
	switch ( lua_type( Lfrom, i )) {
		case LUA_TBOOLEAN:
			lua_pushboolean( Lto, lua_toboolean( Lfrom, i ));
			break;
		
		case LUA_TNUMBER:
			copynumber( Lto, Lfrom, i );
			break;
		
		case LUA_TSTRING:
			str = lua_tolstring( Lfrom, i, &len );
			lua_pushlstring( Lto, str, len );
			break;
		
		case LUA_TNIL:
			lua_pushnil( Lto );
			break;		
		
		case LUA_TTABLE:
			if(!transferTable(Lfrom, i, Lto, type_, 1))
				result = FALSE;
			break;

		case LUA_TUSERDATA:			      
			if(!transferUdata(Lfrom, i, Lto, type_))
				result = FALSE;
			break;
			
		case LUA_TFUNCTION:
			if(!transferFunction(Lfrom, i, Lto, type_))
				result = FALSE;
			break;
		
		default: //leaves error messages when attempting to transfer types not supported (threads, corroutines)
			
			if(type_ == from_temp){
				lua_pop(Lto, 1);
				lua_pushnil( Lto );
				lua_pushfstring( Lto, "failed to send value of unsupported type '%s'", luaL_typename( Lfrom, i ));
			}
			else{
				lua_pushnil( Lfrom );
				lua_pushfstring( Lfrom, "failed to send value of unsupported type '%s'", luaL_typename( Lfrom, i ));
			}
			
			result = FALSE;
	}
	
	return result;
}

/* copies values between lua states' stacks, in synchronous sending */
static int luaproc_copyvalues( lua_State *Lfrom, lua_State *Lto, enum t_transfer type_) {

	int i;
	int n = lua_gettop( Lfrom );
	const char *str;
	size_t len;

	/* ensure there is space in the receiver's stack */
	if ( lua_checkstack( Lto, n ) == 0 ) {
		lua_pushnil( Lto );
		lua_pushstring( Lto, "not enough space in the stack" );
		lua_pushnil( Lfrom );
		lua_pushstring( Lfrom, "not enough space in the receiver's stack" );
		return FALSE;
	}
	
	//type_ may be either to_normal or from_normal
	
	/* test each value's type and, if it's supported, copy value */
	for ( i = 2; i <= n; i++ ) {
		switch ( lua_type( Lfrom, i )) {
			case LUA_TBOOLEAN:
				lua_pushboolean( Lto, lua_toboolean( Lfrom, i ));
				break;
			
			case LUA_TNUMBER:
				copynumber( Lto, Lfrom, i );
				break;
			
			case LUA_TSTRING:
				str = lua_tolstring( Lfrom, i, &len );
				lua_pushlstring( Lto, str, len );
				break;
			
			case LUA_TNIL:
				lua_pushnil( Lto );
				break;		
			
			case LUA_TTABLE:
				if(!transferTable(Lfrom, i, Lto, type_, 1))
					return FALSE;
				break;

			case LUA_TUSERDATA:			      
				if(!transferUdata(Lfrom, i, Lto, type_))
					return FALSE;
				break;
				
			case LUA_TFUNCTION:
				if(!transferFunction(Lfrom, i, Lto, type_))
					return FALSE;
				break;
			
			default:
				lua_settop( Lto, 1 );
				lua_pushnil( Lto );
				lua_pushfstring( Lto, "failed to receive value of unsupported type '%s'", luaL_typename( Lfrom, i ));
				lua_pushnil( Lfrom );
				lua_pushfstring( Lfrom, "failed to send value of unsupported type '%s'", luaL_typename( Lfrom, i ));
				return FALSE;
		}
	}
	
	return TRUE;
}


/* 
copies values to and from a container Lua state

params:

Lfrom	: sender Lua state
Lto		: receiver Lua state
type_	: kind of message transfer

return values:

TRUE	: all values were transferred sucessfully
FALSE	: otherwise

*/

static int luaproc_async_copyvalues( lua_State *Lfrom, lua_State *Lto, enum t_transfer type_) {

	int i;
	
	//number of values to be transferred
	int n_elem_to_copy = 0;
	
	//number of messages in the container Lua state, each message is stored in a container Lua state as a table
	int temp_stack_len = 0;
	
	//return value
	int result = TRUE;
	
	//"type_" may be either "to_temp" or "from_temp"
	if(type_ == to_temp){
		
		//getting the total of values to be transferred from the sender's stack
		n_elem_to_copy = lua_gettop( Lfrom );
		
		//getting the total of message stored in the container Lua state
		temp_stack_len = lua_gettop( Lto );
	}
	else{
		//getting the total number of values to be transferred from the table at the beginning of the container Lua state stack's 
		n_elem_to_copy = lua_rawlen(Lfrom, 1);
		
		//getting the total number of messages stored in the container Lua state
		temp_stack_len = lua_gettop( Lfrom );
	}
	
	//checks if there is enough space in the receiver's stack
	if ( lua_checkstack( Lto, n_elem_to_copy ) == 0 ) {
		
		if(type_ == from_temp){
			lua_pushnil( Lto );
			lua_pushstring( Lto, "not enough space in the stack" );
		}
		else{
			lua_pushnil( Lfrom );
			lua_pushstring( Lfrom, "not enough space in the receiver's stack" );
		}
		
		return FALSE;
	}
	
	//copying a message to a container Lua state
	if(type_ == to_temp){
		
		//creates the table storing the values of the transferred message
		lua_newtable(Lto);
		
		for ( i = 2; i <= n_elem_to_copy; i++ ) {
			lua_pushinteger(Lto, i - 1);
			
			//copies the value to the container Lua state's stack
			result = copy_one_value(Lfrom, i, Lto, type_);
			
			if(result == FALSE)
				break;
			
			//the values composing a message are stored in this table preserving the order they have
			lua_rawset(Lto, -3);
		}
	}
	else{
		
		//copying a message from a container Lua state
		for ( i = 1; i <= n_elem_to_copy; i++ ) {
			
			//getting the values composing a message in the order in which they were stored
			lua_pushinteger(Lfrom, i );
			lua_rawget(Lfrom, 1);
			
			//copies the value to the receiver's stack
			result = copy_one_value(Lfrom, temp_stack_len + 1, Lto, type_);

			if(result == FALSE)
				break;
			
			//removes the value from the stack
			lua_pop(Lfrom, 1);
		}
	}
	
	if(result == FALSE){
		
		//if an error ocurrs during the transfer, this method removes all the values temporary placed in the stack of the container Lua state
		//in this way, a failed transfer does not cause the stack of a container Lua state to become inconsistent
		if(type_ == to_temp)
			lua_pop(Lto, lua_gettop( Lto ) - temp_stack_len);
		else
			lua_pop(Lfrom, lua_gettop( Lfrom ) - temp_stack_len);
		
		return FALSE;
	}
	
	if(type_ == from_temp){
		
		//if no error ocurrs during the transfer, it removes the message transferred from a container Lua state
		lua_remove(Lfrom, 1);
		
		//decreases the counter of async messges not yet received 
		sched_dec_async_msg_count();
	}
	else{
		//if the message was transferred to a container Lua state, it increases the counter of async messges not yet received 
		sched_inc_async_msg_count();
	}
		
	return TRUE;
}

/* 
sends an userdata between Lua state in a predefined way

params:

type_	: kind of message transfer
userdata	: userdata to be sent
Lto		: receiver Lua state

return values:

no values						: if the userdata was sent sucessfully
a nil value plus error messages	: otherwise

*/

static int luaproc_default_send_udata(lua_State *L){

	/* userdata's metatable name */
	const char *mt_name = NULL;

	size_t len;

	/* size of the memory block associated to the userdata to be transferred */
	int size_udata = 0;

	/* validating the function's parameters */
	luaL_checktype(L, 2, LUA_TUSERDATA);
	luaL_checktype(L, 3, LUA_TLIGHTUSERDATA);

	/* getting the function's parameters */

	/* type_ may be either "1" (indicates the userdata is being transferred to another Lua processs) 
	or "3" (indicates the userdata is being transferred to a container Lua state)*/
	int type_ = luaL_checkinteger(L, 1);

	/* getting a pointer to the userdata to be transferred */
	const void *udata_pointer = lua_touserdata(L, 2);

	/* getting a pointer to the receiver Lua state */
	lua_State *Lto = (lua_State *)lua_touserdata(L, 3);

	/* getting the userdata's metatable name in the sender Lua state */
	lua_getmetatable(L, 2);
	lua_pushstring(L, "__name");
	lua_rawget(L, -2);

	mt_name = lua_tolstring(L, -1, &len);

	/* getting the userdata's metatable in the receiver Lua state */
	luaL_getmetatable(Lto, mt_name);

	if(lua_isnil(Lto, -1)){
		/* userdata's metatable is not registered in the receiver Lua state  */
		
		if(type_ == 1){
			/* userdata is being transferred to another Lua state */
			/* this function must leave error messages in both Lua states */
			lua_pushnil(L);
			lua_pushstring(L, "userdata's metatable must be registered in the receiver Lua process");
			
			lua_settop(Lto, 1);
			lua_pushnil(Lto);
			lua_pushstring(Lto, "userdata's metatable must be registered in the receiver Lua process");
			
			return 2;
		}
		else{
			/* userdata is being transferred to a container Lua state */
			/* this function must create (or get) the metatable to be associated to the resulting userdata */
			lua_pop(Lto, 1);
			luaL_newmetatable(Lto, mt_name);
			
			/* storing the metatable's name, in versions prior to Lua 5.3 */
			#if (LUA_VERSION_NUM < 503)
				lua_pushstring(Lto, "__name");
				lua_pushlstring(Lto, mt_name, len);
				lua_rawset(Lto, -3);
			#endif
		}
	}

	/* getting the size of the memory block associated to the userdata, in the sender Lua state */
	size_udata = lua_rawlen(L, 2);

	/* creating the resulting userdata in the receiver Lua state */
	void *toPointer = lua_newuserdata(Lto, size_udata);

	/* copying the userdata's structure to the receiver Lua state */
	memcpy(toPointer, udata_pointer, size_udata);

	/* associating the corresponding metatable to the resulting userdata  */
	lua_pushvalue(Lto, -2);
	lua_setmetatable(Lto, -2);
	lua_remove(Lto, -2);

	/* if no error occurs, a sender transfer function returns no values */
	return 0;
}


/* 
receives an userdata in a predefined way

params:

type_	: kind of message transfer
udata_idx	: index within the stack of the sender Lua state in which the userdata to be received is stored 
Lto		: receiver Lua state

return values:

the resulting userdata			: if the userdata was received sucessfully
a nil value plus error messages	: otherwise

*/

static int luaproc_default_recv_udata(lua_State *L){

	/* userdata's metatable name */
	const char *mt_name = NULL;

	/* size of the memory block associated to the userdata to be transferred */
	int size_udata = 0;

	size_t len;

	/* validating the function's parameters */
	luaL_checktype(L, 3, LUA_TLIGHTUSERDATA);

	/* getting the function's parameters */

	/* type_ may be either "2" (indicates the userdata is being received from another Lua processs) 
	or "4" (indicates the userdata is being received from a container Lua state) */	
	int type_ = luaL_checkinteger(L, 1);

	/* getting the index within the stack of the sender Lua state in which the userdata to be transferred is stored */
	int udata_idx = luaL_checkinteger(L, 2);

	/* getting a pointer to the sender Lua state */
	lua_State *Lfrom  = (lua_State *)lua_touserdata(L, 3);

	/* getting a pointer to the userdata to be transferred */
	const void *udata_pointer = lua_touserdata(Lfrom, udata_idx);

	/* getting the userdata's metatable name in the sender Lua state */
	lua_getmetatable(Lfrom, udata_idx);
	lua_pushstring(Lfrom, "__name");
	lua_rawget(Lfrom, -2);

	mt_name = lua_tolstring(Lfrom, -1, &len);

	/* getting the userdata's metatable in the receiver Lua state */
	luaL_getmetatable(L, mt_name);

	if(lua_isnil(L, -1)){
		/* userdata's metatable is not registered in the receiver Lua state */
		
		if(type_ == 2){
			/* userdata is being received from another Lua process */
			/* this function must leave error messages in both Lua processes */
			lua_pushnil(Lfrom);
			lua_pushstring(Lfrom, "userdata's metatable must be registered in the receiver Lua process");
		}
		
		/* if the userdata is being received from a container Lua state */
		/* this function cannot leave an error message in the container Lua state */
		
		lua_settop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "userdata's metatable must be registered");
		
		return 2;
	}

	/* getting the size of the memory block associated to the userdata, in the sender Lua state */
	size_udata = lua_rawlen(Lfrom, udata_idx);

	/* creating the resulting userdata in the receiver Lua state */
	void *toPointer = lua_newuserdata(L, size_udata);

	/* copying the userdata's structure to the receiver Lua state */
	memcpy(toPointer, udata_pointer, size_udata);

	/* associating the corresponding metatable to the resulting userdata */
	lua_pushvalue(L, -2);
	lua_setmetatable(L, -2);
	lua_remove(L, -2);

	/* leaving nothing in the sender Lua state */
	lua_pop(Lfrom, 2);

	/* if no error occurs, a receiver transfer function returns the resulting userdata */
	return 1;
}

/* 
sends a Luasocket's socket between Lua states

params:

type_	: kind of message transfer
userdata	: socket to be sent
Lto		: receiver Lua state

return values:

no values						: if the socket was sent sucessfully
a nil value plus error messages	: otherwise

*/

static int luaproc_send_socket(lua_State *L){

	/* userdata's metatable name */
	const char *mt_name = NULL;

	size_t len;

	/* size of the memory block associated to the userdata to be transferred */
	int size_udata = 0;

	/* validating the function's parameters */
	luaL_checktype(L, 2, LUA_TUSERDATA);
	luaL_checktype(L, 3, LUA_TLIGHTUSERDATA);

	/* getting the function's parameters */

	/* type_ may be either "1" (indicates the userdata is being transferred to another Lua processs) 
	or "3" (indicates the userdata is being transferred to a container Lua state)*/
	int type_ = luaL_checkinteger(L, 1);

	/* getting a pointer to the userdata to be transferred */
	const void *udata_pointer = lua_touserdata(L, 2);

	/* getting a pointer to the receiver Lua state */
	lua_State *Lto = (lua_State *)lua_touserdata(L, 3);

	/* getting the userdata's metatable name in the sender Lua state */
	lua_getmetatable(L, 2);
	lua_pushstring(L, "__name");
	lua_rawget(L, -2);

	mt_name = lua_tolstring(L, -1, &len);

	/* getting the userdata's metatable in the receiver Lua state */
	luaL_getmetatable(Lto, mt_name);

	if(lua_isnil(Lto, -1)){
		/* userdata's metatable is not registered in the receiver Lua state  */
		
		if(type_ == 1){
			/* userdata is being transferred to another Lua state */
			/* this function must leave error messages in both Lua states */
			lua_pushnil(L);
			lua_pushstring(L, "userdata's metatable must be registered in the receiver Lua process");
			
			lua_settop(Lto, 1);
			lua_pushnil(Lto);
			lua_pushstring(Lto, "userdata's metatable must be registered in the receiver Lua process");
			
			return 2;
		}
		else{
			/* userdata is being transferred to a container Lua state */
			/* this function must create (or get) the metatable to be associated to the resulting userdata */
			lua_pop(Lto, 1);
			luaL_newmetatable(Lto, mt_name);
			
			/* storing the metatable's name, in versions prior to Lua 5.3 */
			#if (LUA_VERSION_NUM < 503)
				lua_pushstring(Lto, "__name");
				lua_pushlstring(Lto, mt_name, len);
				lua_rawset(Lto, -3);
			#endif
		}
	}

	/* getting the size of the memory block associated to the userdata, in the sender Lua state */
	size_udata = lua_rawlen(L, 2);

	/* creating the resulting userdata in the receiver Lua state */
	p_tcp tcp = (p_tcp)lua_newuserdata(Lto, size_udata);

	/* copying the userdata's structure to the receiver Lua state */
	memcpy((void *)tcp, udata_pointer, size_udata);

	/* updating the structure of the resulting userdata */
	p_io io = &tcp->io;
	io->ctx = &tcp->sock;

	p_buffer buf = &tcp->buf;
	buf->io = io;
	buf->tm = &tcp->tm;

	/* associating the corresponding metatable to the resulting userdata  */
	lua_pushvalue(Lto, -2);
	lua_setmetatable(Lto, -2);
	lua_remove(Lto, -2);

	/* if no error occurs, a sender transfer function returns no values */
	return 0;
}

/* 
receives a Luasocket's socket

params:

type_	: kind of message transfer
udata_idx	: index within the stack of the sender Lua state in which the socket to be received is stored 
Lto		: receiver Lua state

return values:

the resulting userdata			: if the socket was received sucessfully
a nil value plus error messages	: otherwise

*/

static int luaproc_recv_socket(lua_State *L){

	/* userdata's metatable name */
	const char *mt_name = NULL;

	/* size of the memory block associated to the userdata to be transferred */
	int size_udata = 0;

	size_t len;

	/* validating the function's parameters */
	luaL_checktype(L, 3, LUA_TLIGHTUSERDATA);

	/* getting the function's parameters */

	/* type_ may be either "2" (indicates the userdata is being received from another Lua processs) 
	or "4" (indicates the userdata is being received from a container Lua state) */	
	int type_ = luaL_checkinteger(L, 1);

	/* getting the index within the stack of the sender Lua state in which the userdata to be transferred is stored */
	int udata_idx = luaL_checkinteger(L, 2);

	/* getting a pointer to the sender Lua state */
	lua_State *Lfrom  = (lua_State *)lua_touserdata(L, 3);

	/* getting a pointer to the userdata to be transferred */
	const void *udata_pointer = lua_touserdata(Lfrom, udata_idx);

	/* getting the userdata's metatable name in the sender Lua state */
	lua_getmetatable(Lfrom, udata_idx);
	lua_pushstring(Lfrom, "__name");
	lua_rawget(Lfrom, -2);

	mt_name = lua_tolstring(Lfrom, -1, &len);

	/* getting the userdata's metatable in the receiver Lua state */
	luaL_getmetatable(L, mt_name);

	if(lua_isnil(L, -1)){
		/* userdata's metatable is not registered in the receiver Lua state */
		
		if(type_ == 2){
			/* userdata is being received from another Lua process */
			/* this function must leave error messages in both Lua processes */
			lua_pushnil(Lfrom);
			lua_pushstring(Lfrom, "userdata's metatable must be registered in the receiver Lua process");
		}
		
		/* if the userdata is being received from a container Lua state */
		/* this function cannot leave an error message in the container Lua state */
		
		lua_settop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "userdata's metatable must be registered");
		
		return 2;
	}

	/* getting the size of the memory block associated to the userdata, in the sender Lua state */
	size_udata = lua_rawlen(Lfrom, udata_idx);

	/* creating the resulting userdata in the receiver Lua state */
	p_tcp tcp = (p_tcp)lua_newuserdata(L, size_udata);

	/* copying the userdata's structure to the receiver Lua state */
	memcpy((void *)tcp, udata_pointer, size_udata);

	/* updating the structure of the resulting userdata */
	p_io io = &tcp->io;
	io->ctx = &tcp->sock;

	p_buffer buf = &tcp->buf;
	buf->io = io;
	buf->tm = &tcp->tm;

	/* associating the corresponding metatable to the resulting userdata */
	lua_pushvalue(L, -2);
	lua_setmetatable(L, -2);
	lua_remove(L, -2);

	/* leaving nothing in the sender Lua state */
	lua_pop(Lfrom, 2);

	/* if no error occurs, a receiver transfer function returns the resulting userdata */
	return 1;
}

/* 
sends a Lua file between Lua states

params:

type_	: kind of message transfer
userdata	: file to be sent
Lto		: receiver Lua state

return values:

no values						: if the file was sent sucessfully
a nil value plus error messages	: otherwise

*/

static int luaproc_send_file(lua_State *L){

	/* userdata's metatable name */
	const char *mt_name = "FILE*";
	int size_udata = 0;
	
	//type_ may be either to_normal(1) or to_temp(3)
	
	/* validating the function's parameters */
	luaL_checktype(L, 2, LUA_TUSERDATA);
	luaL_checktype(L, 3, LUA_TLIGHTUSERDATA);

	/* getting the function's parameters */

	/* type_ may be either "1" (indicates the userdata is being transferred to another Lua processs) 
	or "3" (indicates the userdata is being transferred to a container Lua state)*/
	int type_ = luaL_checkinteger(L, 1);

	/* getting a pointer to the userdata to be transferred */
	const void *udata_pointer = lua_touserdata(L, 2);

	/* getting a pointer to the receiver Lua state */
	lua_State *Lto = (lua_State *)lua_touserdata(L, 3);

	/* getting the userdata's metatable in the receiver Lua state */
	luaL_getmetatable(Lto, mt_name);

	if(lua_isnil(Lto, -1)){
		/* userdata's metatable is not registered in the receiver Lua state  */
		
		if(type_ == 1){
			/* userdata is being transferred to another Lua state */
			/* this function must leave error messages in both Lua states */
			lua_pushnil(L);
			lua_pushstring(L, "userdata's metatable must be registered in the receiver Lua process");
			
			lua_settop(Lto, 1);
			lua_pushnil(Lto);
			lua_pushstring(Lto, "userdata's metatable must be registered in the receiver Lua process");
			
			return 2;
		}
		else{
			/* userdata is being transferred to a container Lua state */
			/* this function must create (or get) the metatable to be associated to the resulting userdata */
			lua_pop(Lto, 1);
			luaL_newmetatable(Lto, mt_name);
			
			/* storing the metatable's name, in versions prior to Lua 5.3 */
			#if (LUA_VERSION_NUM < 503)
				lua_pushstring(Lto, "__name");
				lua_pushstring(Lto, mt_name);
				lua_rawset(Lto, -3);
			#endif
		}
	}
	
	/* getting the size of the memory block associated to the userdata, in the sender Lua state */
	size_udata = lua_rawlen(L, 2);

	/* creating the resulting userdata in the receiver Lua state */
	void *fhandler = lua_newuserdata(Lto, size_udata);

	/* copying the userdata's structure to the receiver Lua state */
	memcpy(fhandler, udata_pointer, size_udata);

	/* associating the corresponding metatable to the resulting userdata  */
	lua_pushvalue(Lto, -2);
	lua_setmetatable(Lto, -2);
	lua_remove(Lto, -2);

	/* if no error occurs, a sender transfer function returns no values */
	return 0;
}

/* 
receives a Lua file

params:

type_	: kind of message transfer
udata_idx	: index within the stack of the sender Lua state in which the file to be received is stored 
Lto		: receiver Lua state

return values:

the resulting userdata			: if the file was received sucessfully
a nil value plus error messages	: otherwise

*/

static int luaproc_recv_file(lua_State *L){

	/* userdata's metatable name */
	const char *mt_name = "FILE*";

	/* size of the memory block associated to the userdata to be transferred */
	int size_udata = 0;

	/* validating the function's parameters */
	luaL_checktype(L, 3, LUA_TLIGHTUSERDATA);

	/* getting the function's parameters */

	/* type_ may be either "2" (indicates the userdata is being received from another Lua processs) 
	or "4" (indicates the userdata is being received from a container Lua state) */	
	int type_ = luaL_checkinteger(L, 1);

	/* getting the index within the stack of the sender Lua state in which the userdata to be transferred is stored */
	int udata_idx = luaL_checkinteger(L, 2);

	/* getting a pointer to the sender Lua state */
	lua_State *Lfrom  = (lua_State *)lua_touserdata(L, 3);

	/* getting a pointer to the userdata to be transferred */
	const void *udata_pointer = lua_touserdata(Lfrom, udata_idx);

	/* getting the userdata's metatable in the receiver Lua state */
	luaL_getmetatable(L, mt_name);

	if(lua_isnil(L, -1)){
		/* userdata's metatable is not registered in the receiver Lua state */
		
		if(type_ == 2){
			/* userdata is being received from another Lua process */
			/* this function must leave error messages in both Lua processes */
			lua_pushnil(Lfrom);
			lua_pushstring(Lfrom, "userdata's metatable must be registered in the receiver Lua process");
		}
		
		/* if the userdata is being received from a container Lua state */
		/* this function cannot leave an error message in the container Lua state */
		
		lua_settop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "userdata's metatable must be registered");
		
		return 2;
	}

	/* getting the size of the memory block associated to the userdata, in the sender Lua state */
	size_udata = lua_rawlen(Lfrom, udata_idx);

	/* creating the resulting userdata in the receiver Lua state */
	void *fhandler = lua_newuserdata(L, size_udata);

	/* copying the userdata's structure to the receiver Lua state */
	memcpy(fhandler, udata_pointer, size_udata);

	/* associating the corresponding metatable to the resulting userdata */
	lua_pushvalue(L, -2);
	lua_setmetatable(L, -2);
	lua_remove(L, -2);

	/* if no error occurs, a receiver transfer function returns the resulting userdata */
	return 1;
}

/*********************
 * library functions *
 *********************/

/* set maximum number of lua processes in the recycle list */
static int luaproc_recycle_set( lua_State *L ) {

  luaproc *lp;

  /* validate parameter is a non negative number */
  lua_Integer max = luaL_checkinteger( L, 1 );
  luaL_argcheck( L, max >= 0, 1, "recycle limit must be positive" );

  /* get exclusive access to recycled lua processes list */
  pthread_mutex_lock( &mutex_recycle_list );

  recyclemax = max;  /* set maximum number */

  /* remove extra nodes and destroy each lua processes */
  while ( list_count( &recycle_list ) > recyclemax ) {
    lp = list_remove( &recycle_list );
    lua_close( lp->lstate );
  }
  /* release exclusive access to recycled lua processes list */
  pthread_mutex_unlock( &mutex_recycle_list );

  return 0;
}

/* wait until there are no more active lua processes */
static int luaproc_wait( lua_State *L ) {
  sched_wait();
  return 0;
}

/* set number of workers (creates or destroys accordingly) */
static int luaproc_set_numworkers( lua_State *L ) {

  /* validate parameter is a positive number */
  lua_Integer numworkers = luaL_checkinteger( L, -1 );
  luaL_argcheck( L, numworkers > 0, 1, "number of workers must be positive" );

  /* set number of threads; signal error on failure */
  if ( sched_set_numworkers( numworkers ) == LUAPROC_SCHED_PTHREAD_ERROR ) {
      luaL_error( L, "failed to create worker" );
  } 

  return 0;
}

/* return the number of active workers */
static int luaproc_get_numworkers( lua_State *L ) {
  lua_pushnumber( L, sched_get_numworkers( ));
  return 1;
}

/* create and schedule a new lua process */
static int luaproc_create_newproc( lua_State *L ) {

  size_t len;
  luaproc *lp;
  luaL_Buffer buff;
  const char *code;
  int d;
  int lt = lua_type( L, 1 );

  /* check function argument type - must be function or string; in case it is
     a function, dump it into a binary string */
  if ( lt == LUA_TFUNCTION ) {
    lua_settop( L, 1 );
    luaL_buffinit( L, &buff );
    d = dump( L, luaproc_buff_writer, &buff, FALSE );
    if ( d != 0 ) {
      lua_pushnil( L );
      lua_pushfstring( L, "error %d dumping function to binary string", d );
      return 2;
    }
    luaL_pushresult( &buff );
    lua_insert( L, 1 );
  } else if ( lt != LUA_TSTRING ) {
    lua_pushnil( L );
    lua_pushfstring( L, "cannot use '%s' to create a new process",
                     luaL_typename( L, 1 ));
    return 2;
  }

  /* get pointer to code string */
  code = lua_tolstring( L, 1, &len );

  /* get exclusive access to recycled lua processes list */
  pthread_mutex_lock( &mutex_recycle_list );

  /* check if a lua process can be recycled */
  if ( recyclemax > 0 ) {
    lp = list_remove( &recycle_list );
    /* otherwise create a new lua process */
    if ( lp == NULL ) {
      lp = luaproc_new( L );
    }
  } else {
    lp = luaproc_new( L );
  }

  /* release exclusive access to recycled lua processes list */
  pthread_mutex_unlock( &mutex_recycle_list );

  /* init lua process */
  lp->status = LUAPROC_STATUS_IDLE;
  lp->args   = 0;
  lp->chan   = NULL;

  /* load code in lua process */
  luaproc_loadbuffer( L, lp->lstate, code, len );

  /* if lua process is being created from a function, copy its upvalues and
     remove dumped binary string from stack */
  if ( lt == LUA_TFUNCTION ) {
    if ( luaproc_copyupvalues( L, lp->lstate, 2, to_normal) == FALSE ) {
      luaproc_recycle_insert( lp ); 
      return 2;
    }
    lua_pop( L, 1 );
  }

  sched_inc_lpcount();   /* increase active lua process count */
  sched_queue_proc( lp );  /* schedule lua process for execution */
  lua_pushboolean( L, TRUE );

  return 1;
}

/* 
registers transfer functions for userdata

params:

a table storing a "transffuncs" C function, which returns a table storing transfer functions for userdata

return values:

TRUE						: if the given tranfer functions were registered sucessfully
a nil value plus error messages	: otherwise

*/

static int luaproc_regudata(lua_State *L){
	
	const char *transf_funcs = "transffuncs";
	int top = lua_gettop(L);
		
	//it checks whether the parameters is a table
	if(lua_type(L, 1) != LUA_TTABLE){
		lua_pushnil(L);
		lua_pushstring(L, "the first parameter must be a table");
		return 2;
	}
	
	//attempts to get the "transffuncs" C function
	lua_pushstring(L, transf_funcs);
	lua_rawget(L, 1);
	
	//it checks whether the obtained value is a C function
	if(lua_tocfunction(L, -1) == NULL){
		lua_pushnil(L);
		lua_pushstring(L, "table passed as a parameter has no a -transf_funcs- C function");
		return 2;
	}
	
	//executes the "transffuncs" C function
	lua_pcall(L, 0, LUA_MULTRET, 0);
			
	//it checks whether the "transffuncs" C function leaves the table storing transfer functions onto the stack
	if(lua_type(L, -1) != LUA_TTABLE || lua_gettop(L) != top + 1){
		lua_pushnil(L);
		lua_pushstring(L, "the -transf_funcs- function must only leave a table when executed");
		return 2;
	}
	
	//gets the "transferable_udata" table, which is used for storing the transfer functions
	//the keys and values of this table are userdata metatable names and tables storing transfer functions, respectively 
	lua_pushlightuserdata(L, (void *)transferable_udata);
	lua_rawget(L, LUA_REGISTRYINDEX);
	
	if(lua_isnil(L, -1)){
		lua_pop(L, 1);
		
		//creates the "transferable_udata" table, if not exist
		lua_newtable(L);
		lua_pushlightuserdata(L, (void *)transferable_udata);
		lua_pushvalue(L, -2);
		lua_rawset(L, LUA_REGISTRYINDEX);
	}
	
	//it checks whether the table returned by the "transffuncs" function stores transfer functions for an userdata previously registered 
	lua_pushnil(L);
	while(lua_next(L, -3) != 0){
	
		if(lua_type(L, -2) == LUA_TSTRING){
			
			lua_pushvalue(L, -2);
			lua_rawget(L, -4);
			
			if(!lua_isnil(L, -1)){
				
				//If so, this method registers no transfer function and returns a nil value plus an error message
				lua_pushnil(L);
				lua_pushfstring(L, "userdata - %s - already registered", lua_tostring(L, -4));
				return 2;
			}
			
			lua_pop(L, 1);
		}
		else{
			lua_pushnil(L);
			lua_pushstring(L, "the table returned by transf_funcs must be indexed by metatable names");
			return 2;
		}
		
		lua_pop(L, 1);
	}
	
	//otherwise, it registers all the transfer functions in the "transferable_udata" table
	lua_pushnil(L);
	while(lua_next(L, -3) != 0){
		lua_pushvalue(L, -2);
		lua_insert(L, -2);
		lua_rawset(L, -4);
	}
	
	lua_pushboolean(L, TRUE);
	return 1;
}

/* 
this function is executed when a method call on a transferred userdata ocurrs

params:

method's name (it is not used by this function)

return values:

a function returning a nil value plus an error message when executed

*/

static int luaproc_denied_udata (lua_State *L){
	lua_pushstring(L, "Access denied, attemp to index an userdata tranferred.");
	lua_pushcclosure(L, luaproc_err_udata, 1);
	
	return 1;
}

//function returned by the above function
static int luaproc_err_udata (lua_State *L){
	const char *errMsg = lua_tostring(L, lua_upvalueindex(1));
	lua_pushnil(L);
	lua_pushstring(L, errMsg);
	return 2;
}

// a "transffuncs" function incorporated to Luaproc to allow transferring Lua files and Luasocket's sockets in a predefined way
static int luaproc_transf_funcs(lua_State *L){
	
	const struct luaL_Reg socket_transf_funcs[] = {
		{ "send", luaproc_send_socket},
		{ "recv", luaproc_recv_socket},
		{ NULL, NULL }
	};
	
	const struct luaL_Reg file_transf_funcs[] = {
		{ "send", luaproc_send_file},
		{ "recv", luaproc_recv_file},
		{ NULL, NULL }
	};
	
	//creates the table returned by this function
	lua_newtable(L);
	
	//registers in the above table the transfer functions for Lua files
	lua_pushstring(L, "FILE*");
	luaL_newlib(L, file_transf_funcs);
	lua_rawset(L, -3);
	
	//registers in the above table the transfer functions for Luasocket's sockets
	luaL_newlib(L, socket_transf_funcs);
	
	lua_pushstring(L, "tcp{master}");
	lua_pushvalue(L, -2);
	lua_rawset(L, -4);
	
	lua_pushstring(L, "tcp{client}");
	lua_pushvalue(L, -2);
	lua_rawset(L, -4);
	
	lua_pushstring(L, "tcp{server}");
	lua_pushvalue(L, -2);
	lua_rawset(L, -4);
	
	lua_pop(L, 1);
	
	//return a table storing transfer functions for Lua files and Luasocket's sockets 
	return 1;
}

/* 
performs a barrier operation on the specified channel

params:

chname	: channel's name
n_elems	: number of Lua processes involved in the operation

return values:

TRUE						: once all the Lua processes involved in the operation have reached this channel
a nil value plus error messages	: if the channel specified in the call does not exist

*/
static int luaproc_barrier(lua_State *L){
	
	//name of the channel on which the operation is performed
	const char *chname = luaL_checkstring( L, 1 );
	
	//number of Lua processes involved in the operation
	int n_elems = luaL_checkinteger( L, 2 );
	
	luaproc *self = NULL;
	
	//gets the Lua process executing this function
	if (L == mainlp.lstate)
		self = &mainlp;
	else
		self = luaproc_getself( L );
	
	//gets and locks the channel on which the operation will be performed
	channel *ch = channel_locked_get(chname);
	
	/* if channel is not found, return an error to lua */
	if ( ch == NULL ) {
		lua_pushnil( L );
		lua_pushfstring( L, "channel '%s' does not exist", chname );
		return 2;
	}
	
	//checks whether a barrier operation is being performed on this channel
	if(ch->barrier == NULL){
		
		//If not, allocates memory for the structure used for handling a barrier operation
		ch->barrier = malloc(sizeof(struct stbarrier));
		
		//initializes the queue storing the involved Lua processes
		list_init(&ch->barrier->elems);
		
		//main Lua process is stored (if involved) in this field in a separate way to the others
		ch->barrier->mainlp = NULL;
		
		//indicates the number of Lua processes that have to call this function on this channel to complete the operation
		//note that a barrier operation is initialized with the number of Lua processes indicated by the first process in reaching this channel
		ch->barrier->num_elem = n_elems;
	}

	//gets the number of Lua process, which have already reached this channel
	int num_lp = list_count(&ch->barrier->elems) + (ch->barrier->mainlp != NULL ? 1 : 0); 
	
	//checks whether the Lua process executing this function is the last one in reaching the barrier
	if(num_lp < ch->barrier->num_elem - 1){
		
		//If not, this process must wait for the others to arrive
		lua_pushboolean(self->lstate, TRUE);
		self->args = 1;
		
		if ( self->lstate == mainlp.lstate ){
			
			//in case this is the main Lua process
			ch->barrier->mainlp = self;
			
			pthread_mutex_lock( &mutex_mainls );
			luaproc_unlock_channel( ch );
			pthread_cond_wait(&cond_mainls_sendrecv, &mutex_mainls);
			pthread_mutex_unlock( &mutex_mainls );
			return mainlp.args;
		}
		
		//inserts this Lua process in the queue storing the Lua processes involved in the operation
		list_insert(&ch->barrier->elems, self);
		
		//indicates this Lua process will block while performing a barrier operation
		self->status = LUAPROC_BLOCKED_BARRIER;
		
		//indicates the channel Luaproc must unlock after releasing the thread executing this Lua process
		self->chan = ch;
		
		//releases the worker thread executing this Lua process
		return lua_yield( L, lua_gettop( L ));
	}

	//if this Lua process is the last one in reaching the barrier, it resumes the execution of the Lua processes blocked in this channel
	lua_pushboolean(self->lstate, TRUE);
	
	sched_queue_list_proc(&ch->barrier->elems);
	
	if(ch->barrier->mainlp != NULL){
		pthread_mutex_lock( &mutex_mainls );
		pthread_cond_signal( &cond_mainls_sendrecv );
		pthread_mutex_unlock( &mutex_mainls );
	}
	
	//free the strucure used for handling the barrier operation
	//every time a barrier operation is performed, a new barrier structure is created
	free(ch->barrier);
	ch->barrier = NULL;
	
	//releases the channel on which the barrier operation was performed
	luaproc_unlock_channel( ch );
	
	return 1;
}

// sends a message either synchronously or asynchronously 
static int luaproc_send( lua_State *L ) {

	int ret;
	channel *chan;
	luaproc *dstlp, *self;
	const char *chname = luaL_checkstring( L, 1 );

	chan = channel_locked_get( chname );
	/* if channel is not found, return an error to lua */
	if ( chan == NULL ) {
		lua_pushnil( L );
		lua_pushfstring( L, "channel '%s' does not exist", chname );
		return 2;
	}	

	/* remove first lua process, if any, from channel's receive list */
	dstlp = list_remove( &chan->recv );

	if ( dstlp != NULL ) { /* found a receiver? */
		/* unlock channel access */
		luaproc_unlock_channel( chan );
		
		/* try to move values between lua states' stacks */
		ret = luaproc_copyvalues( L, dstlp->lstate, to_normal);
		/* -1 because channel name is on the stack */
		dstlp->args = lua_gettop( dstlp->lstate ) - 1; 
		if ( dstlp->lstate == mainlp.lstate ) {
			/* if sending process is the parent (main) Lua state, unblock it */
			pthread_mutex_lock( &mutex_mainls );
			pthread_cond_signal( &cond_mainls_sendrecv );
			pthread_mutex_unlock( &mutex_mainls );
		} else {
			/* schedule receiving lua process for execution */
			sched_queue_proc( dstlp );
		}
		/* unlock channel access */
		//luaproc_unlock_channel( chan );
		if ( ret == TRUE ) { /* was send successful? */
			lua_pushboolean( L, TRUE );
			return 1;
		} else { /* nil and error msg already in stack */
			return 2;
		}

	}
	//if there is no a matching receiver Lua process
	else if(chan->type == 0){
		
		//in a synchronous sending, this Lua process will block
		if ( L == mainlp.lstate ) {
			/* sending process is the parent (main) Lua state - block it */
			mainlp.chan = chan;
			luaproc_queue_sender( &mainlp );
			luaproc_unlock_channel( chan );
			pthread_mutex_lock( &mutex_mainls );
			pthread_cond_wait( &cond_mainls_sendrecv, &mutex_mainls );
			pthread_mutex_unlock( &mutex_mainls );
			return mainlp.args;
		} else {
			/* sending process is a standard luaproc - set status, block and yield */
			self = luaproc_getself( L );
			if ( self != NULL ) {
				self->status = LUAPROC_STATUS_BLOCKED_SEND;
				self->chan   = chan;
			}
			/* yield. channel will be unlocked by the scheduler */
			return lua_yield( L, lua_gettop( L ));
		}
	}
	else{
		
		//in an asynchronous sending, this Lua process must copy the message to a container Lua state
		ret = luaproc_async_copyvalues( L, chan->lstate, to_temp);
		
		//after copying the message, it releases the channel
		luaproc_unlock_channel( chan );
		
		if ( ret == TRUE ) { /* was store successful? */
			lua_pushboolean( L, TRUE );
			return 1;
		} else { /* nil and error msg already in stack */
			return 2;
		}
	}
}

/* receives a message sent either synchronously or asynchronously */
static int luaproc_receive( lua_State *L ) {

	int ret, nargs;
	channel *chan;
	luaproc *srclp, *self;
	const char *chname = luaL_checkstring( L, 1 );

	/* get number of arguments passed to function */
	nargs = lua_gettop( L );

	chan = channel_locked_get( chname );
	/* if channel is not found, return an error to Lua */
	if ( chan == NULL ) {
		lua_pushnil( L );
		lua_pushfstring( L, "channel '%s' does not exist", chname );
		return 2;
	}
	
	//in synchronous sending
	if(chan->type == 0){

		/* remove first lua process, if any, from channels' send list */
		srclp = list_remove( &chan->send );

		if ( srclp != NULL ) {  /* found a sender? */

			/* unlock channel access */
			luaproc_unlock_channel( chan );
			
			/* try to move values between lua states' stacks */
			ret = luaproc_copyvalues( srclp->lstate, L, from_normal);

			if ( ret == TRUE ) { /* was receive successful? */
				lua_pushboolean( srclp->lstate, TRUE );
				srclp->args = 1;
			} 
			else {  /* nil and error_msg already in stack */
				srclp->args = 2;
			}

			if ( srclp->lstate == mainlp.lstate ) {
				/* if sending process is the parent (main) Lua state, unblock it */
				pthread_mutex_lock( &mutex_mainls );
				pthread_cond_signal( &cond_mainls_sendrecv );
				pthread_mutex_unlock( &mutex_mainls );
			} else {
				/* otherwise, schedule process for execution */
				sched_queue_proc( srclp );
			}

			/* unlock channel access */
			//luaproc_unlock_channel( chan );
			/* disconsider channel name, async flag and any other args passed 
			to the receive function when returning its results */
			return lua_gettop( L ) - nargs; 

		} else {  /* otherwise test if receive was synchronous or asynchronous */
			if ( lua_toboolean( L, 2 )) { /* asynchronous receive */
				/* unlock channel access */
				luaproc_unlock_channel( chan );
				/* return an error */
				lua_pushnil( L );
				lua_pushfstring( L, "no senders waiting on channel '%s'", chname );
				return 2;
			} else { /* synchronous receive */
				
				lua_settop(L, 1);//it must wait only with the channel's name onto its stack
				
				if ( L == mainlp.lstate ) {
					/*  receiving process is the parent (main) Lua state - block it */
					mainlp.chan = chan;
					luaproc_queue_receiver( &mainlp );
					luaproc_unlock_channel( chan );
					pthread_mutex_lock( &mutex_mainls );
					pthread_cond_wait( &cond_mainls_sendrecv, &mutex_mainls );
					pthread_mutex_unlock( &mutex_mainls );
					return mainlp.args;
				} else {
					/* receiving process is a standard luaproc - set status, block and 
					yield */
					self = luaproc_getself( L );
					if ( self != NULL ) {
						self->status = LUAPROC_STATUS_BLOCKED_RECV;
						self->chan   = chan;
					}
					/* yield. channel will be unlocked by the scheduler */
					return lua_yield( L, lua_gettop( L ));
				}
			}
		}
	}
	else{
		
		//in asynchronous sending
		
		//ensures the receiver's stack to store only the channel's name 
		lua_settop(L, 1);
	
		//checks whether the container Lua state stores messages in transit
		if(lua_gettop(chan->lstate) > 0){
			
			//If so, it receives the message at the beginning of the container Lua state's stack
			ret = luaproc_async_copyvalues(chan->lstate, L, from_temp);
			
			//releases this channel
			luaproc_unlock_channel( chan );
			
			if ( ret == TRUE ) { /* was store successful? */
				return lua_gettop( L ) - 1; 
			} else { /* nil and error msg already in stack */
				return 2;
			}
		}
		else{
			
			//if the container Lua state stores no message, this Lua process will block
			if ( L == mainlp.lstate ) {
				/*  receiving process is the parent (main) Lua state - block it */
				mainlp.chan = chan;
				luaproc_queue_receiver( &mainlp );
				pthread_mutex_lock( &mutex_mainls );
				luaproc_unlock_channel( chan );
				pthread_cond_wait( &cond_mainls_sendrecv, &mutex_mainls );
				pthread_mutex_unlock( &mutex_mainls );
				return mainlp.args;
			} else {
				self = luaproc_getself( L );
				if ( self != NULL ) {
					self->status = LUAPROC_STATUS_TMP_RECV;
					self->chan   = chan;
				}
				/* yield. channel will be unlocked by the scheduler */
				return lua_yield( L, lua_gettop( L ));
			}
		}
	}
}

/* create a new channel */
static int luaproc_create_channel( lua_State *L ) {

	const char *chname = luaL_checkstring( L, 1 );
	int type_ch = 0;
	
	//gets the type of channel to be created
	if(lua_gettop(L) > 1 && lua_isboolean(L, 2))
		type_ch = lua_toboolean(L, 2);
	
	channel *chan = channel_locked_get( chname );
	if (chan != NULL) {  /* does channel exist? */
		/* unlock the channel mutex locked by channel_locked_get */
		luaproc_unlock_channel( chan );
		/* return an error to lua */
		lua_pushnil( L );
		lua_pushfstring( L, "channel '%s' already exists", chname );
		return 2;
	} else {  /* create channel */
		channel_create(chname, type_ch);
		lua_pushboolean( L, TRUE );
		return 1;
	}
}

/* destroy a channel */
static int luaproc_destroy_channel( lua_State *L ) {

	channel *chan;
	list *blockedlp;
	luaproc *lp;
	const char *chname = luaL_checkstring( L,  1 );

	/* get exclusive access to channels list */
	pthread_mutex_lock( &mutex_channel_list );

	/*
	try to get channel and lock it; if lock fails, release external
	lock ('mutex_channel_list') to try again when signaled -- this avoids
	keeping the external lock busy for too long. during this release,
	the channel may have been destroyed, so it must try to get it again.
	*/
	while ((( chan = channel_unlocked_get( chname )) != NULL ) &&
		( pthread_mutex_trylock( &chan->mutex ) != 0 )) {
			pthread_cond_wait( &chan->can_be_used, &mutex_channel_list );
	}

	if ( chan == NULL ) {  /* found channel? */
		/* release exclusive access to channels list */
		pthread_mutex_unlock( &mutex_channel_list );
		/* return an error to lua */
		lua_pushnil( L );
		lua_pushfstring( L, "channel '%s' does not exist", chname );
		return 2;
	}

	//checks whether an asynchronous channel still stores messages in transit
	if(chan->type == 1 && lua_gettop(chan->lstate) > 0){
		
		//If so, it returns a nil value plus error messages
		lua_pushnil( L );
		lua_pushfstring( L, "asynchronous channel '%s' still stores messages", chname );
		
		pthread_mutex_unlock( &chan->mutex );
		pthread_cond_signal( &chan->can_be_used );
		pthread_mutex_unlock( &mutex_channel_list );
		
		return 2;
	}

	/* remove channel from table */
	lua_getglobal( chanls, LUAPROC_CHANNELS_TABLE );
	lua_pushnil( chanls );
	lua_setfield( chanls, -2, chname );
	lua_pop( chanls, 1 );

	pthread_mutex_unlock( &mutex_channel_list );

	/*
	wake up workers there are waiting to use the channel.
	they will not find the channel, since it was removed,
	and will not get this condition anymore.
	*/
	pthread_cond_broadcast( &chan->can_be_used );

	/*
	dequeue lua processes waiting on the channel, return an error message
	to each of them indicating channel was destroyed and schedule them
	for execution (unblock them).
	*/
	if ( chan->type == 0 && chan->send.head != NULL ) {
		lua_pushfstring( L, "channel '%s' destroyed while waiting for receiver", chname );
		blockedlp = &chan->send;
	}
	else {
		lua_pushfstring( L, "channel '%s' destroyed while waiting for sender", chname );
		blockedlp = &chan->recv;
	}
	
	while (( lp = list_remove( blockedlp )) != NULL ) {
		/* return an error to each process */
		lua_pushnil( lp->lstate );
		lua_pushstring( lp->lstate, lua_tostring( L, -1 ));
		lp->args = 2;
		sched_queue_proc( lp ); /* schedule process for execution */
	}
	
	//when destroying an asynchronous channel, its contanier Lua state must be closed
	if(chan->type == 1)
		lua_close(chan->lstate);

	/* unlock channel mutex and destroy both mutex and condition */
	pthread_mutex_unlock( &chan->mutex );
	pthread_mutex_destroy( &chan->mutex );
	pthread_cond_destroy( &chan->can_be_used );

	lua_pushboolean( L, TRUE );
	return 1;
}

/***********************
 * get'ers and set'ers *
 ***********************/

/* return the channel where a lua process is blocked at */
channel *luaproc_get_channel( luaproc *lp ) {
  return lp->chan;
}

/* return a lua process' status */
int luaproc_get_status( luaproc *lp ) {
  return lp->status;
}

/* set lua a process' status */
void luaproc_set_status( luaproc *lp, int status ) {
  lp->status = status;
}

/* return a lua process' state */
lua_State *luaproc_get_state( luaproc *lp ) {
  return lp->lstate;
}

/* return the number of arguments expected by a lua process */
int luaproc_get_numargs( luaproc *lp ) {
  return lp->args;
}

/* set the number of arguments expected by a lua process */
void luaproc_set_numargs( luaproc *lp, int n ) {
  lp->args = n;
}


/**********************************
 * register structs and functions *
 **********************************/

static void luaproc_reglualib( lua_State *L, const char *name, 
                               lua_CFunction f ) {
  lua_getglobal( L, "package" );
  lua_getfield( L, -1, "preload" );
  lua_pushcfunction( L, f );
  lua_setfield( L, -2, name );
  lua_pop( L, 2 );
}

static void luaproc_openlualibs( lua_State *L ) {
  requiref( L, "_G", luaopen_base, FALSE );
  requiref( L, "package", luaopen_package, TRUE );
  luaproc_reglualib( L, "io", luaopen_io );
  luaproc_reglualib( L, "os", luaopen_os );
  luaproc_reglualib( L, "table", luaopen_table );
  luaproc_reglualib( L, "string", luaopen_string );
  luaproc_reglualib( L, "math", luaopen_math );
  luaproc_reglualib( L, "debug", luaopen_debug );
#if (LUA_VERSION_NUM == 502)
  luaproc_reglualib( L, "bit32", luaopen_bit32 );
#endif
#if (LUA_VERSION_NUM >= 502)
  luaproc_reglualib( L, "coroutine", luaopen_coroutine );
#endif
#if (LUA_VERSION_NUM >= 503)
  luaproc_reglualib( L, "utf8", luaopen_utf8 );
#endif

}

LUALIB_API int luaopen_luaproc( lua_State *L ) {

	/* register luaproc functions */
	luaL_newlib( L, luaproc_funcs );

	/* wrap main state inside a lua process */
	mainlp.lstate = L;
	mainlp.status = LUAPROC_STATUS_IDLE;
	mainlp.args   = 0;
	mainlp.chan   = NULL;
	mainlp.next   = NULL;
	/* initialize recycle list */
	list_init( &recycle_list );

	/* initialize channels table and lua_State used to store it */
	chanls = luaL_newstate();
	lua_newtable( chanls );
	lua_setglobal( chanls, LUAPROC_CHANNELS_TABLE );
	/* create finalizer to join workers when Lua exits */
	lua_newuserdata( L, 0 );
	lua_setfield( L, LUA_REGISTRYINDEX, "LUAPROC_FINALIZER_UDATA" );
	luaL_newmetatable( L, "LUAPROC_FINALIZER_MT" );
	lua_pushliteral( L, "__gc" );
	lua_pushcfunction( L, luaproc_join_workers );
	lua_rawset( L, -3 );
	lua_pop( L, 1 );
	lua_getfield( L, LUA_REGISTRYINDEX, "LUAPROC_FINALIZER_UDATA" );
	lua_getfield( L, LUA_REGISTRYINDEX, "LUAPROC_FINALIZER_MT" );
	lua_setmetatable( L, -2 );
	lua_pop( L, 1 );

	/* initialize scheduler */
	if ( sched_init() == LUAPROC_SCHED_PTHREAD_ERROR ) {
		luaL_error( L, "failed to create worker" );
	}

	return 1;
}

static int luaproc_loadlib( lua_State *L ) {

  /* register luaproc functions */
  luaL_newlib( L, luaproc_funcs );

  return 1;
}

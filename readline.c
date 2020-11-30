/**
 * See the LICENSE file for the copyright notice
 */

#include <stdlib.h>
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <signal.h>
#include <setjmp.h>

#define LUA_LIB
#include "lauxlib.h"

static int lua_readline(lua_State*);
static void lua_initgenerator(lua_State*, const char*);
static char* gen_function(const char*, int);

static void *REGISTRY_KEY_GENERATOR = (void*) lua_readline;
static void *REGISTRY_KEY_ITERATOR = (void*) lua_initgenerator;
static lua_State *globalL;
static sigjmp_buf globalEnv;

/**
 * An iterator that always returns nil
 */
static int lua_returnnil(lua_State *L)
{
	return 0;
}

/**
 * A generator that always returns an empty iterator
 */
static int lua_niliterator(lua_State *L)
{
	lua_pushcfunction(L, lua_returnnil);
	return 1;
}

/**
 * The iterator function to be returned by lua_ipairsiterator
 * Upvalues:
 * 1-3) The return values from ipairs or the last iterator call
 * 4) The prefix to filter iterated values to start with
 */
static int lua_iterstep(lua_State *L) {
	lua_checkstack(L, 3);
	size_t len;
	/* Get the prefix as string */
	const char *pref = lua_tolstring(L, lua_upvalueindex(4), &len);
	for (;;) {
		/* Simulate the "for" operator behavior */
		lua_pushvalue(L, lua_upvalueindex(1));
		lua_pushvalue(L, lua_upvalueindex(2));
		lua_pushvalue(L, lua_upvalueindex(3));
		lua_call(L, 2, 2);
		/* Exchange the "key" and "value" returned values on stack */
		lua_insert(L, -2);
		/* Store the "key" returned */
		lua_replace(L, lua_upvalueindex(3));
		size_t rlen=0;
		const char *res=NULL;
		/* If nil was returned, we have finished the iteration - forward it */
		if (lua_isnil(L, -1))
			return 1;
		/* Get the string returned */
		res = lua_tolstring(L, -1, &rlen);
		/* Check if it starts with the prefix supplied when creating the iterator */
		if (rlen>=len && strncmp(pref, res, len)==0)
			/* If it matches, return the string */
			return 1;
		/* Clear the unmatched value off the stack and fetch a new one */
		lua_pop(L, 1);
	}
}

/**
 * Generator function that calls ipairs for a given table and wraps the iterator returned by it in a closure
 * Upvalues:
 * 1) The prefix to filter the iterated values to start with
 */
static int lua_ipairsiterator(lua_State *L) {
	lua_checkstack(L, 2);
	lua_getglobal(L, "ipairs");
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_call(L, 1, 3);
	lua_pushvalue(L, 1);
	lua_pushcclosure(L, lua_iterstep, 4);
	return 1;
}

/**
 * Allocates memory to copy a string and copies it
 */
static char* clonestr(const char *srcstr)
{
	size_t len = strlen(srcstr)+1;
	char *dststr = (char*) malloc(len);
	strlcpy(dststr, srcstr, len);
	return dststr;
}

/**
 * Generator init function
 * Calls the generator function to get the iterator function and stores the iterator function to the registry
 */
static void lua_initgenerator(lua_State *L, const char *text)
{
	lua_checkstack(L, 2);
	lua_pushlightuserdata(L, REGISTRY_KEY_GENERATOR);
	lua_gettable(L, LUA_REGISTRYINDEX);
	lua_pushstring(L, text);
	lua_call(L, 1, 1);
	lua_pushlightuserdata(L, REGISTRY_KEY_ITERATOR);
	lua_insert(L, -2);
	lua_settable(L, LUA_REGISTRYINDEX);
}

/**
 * Generator step function
 * Calls the iterator function and returns its return value as string
 */
static char* lua_stepgenerator(lua_State *L)
{
	lua_checkstack(L, 1);
	/* Get the generator function that was stored by lua_readline */
	lua_pushlightuserdata(L, REGISTRY_KEY_ITERATOR);
	lua_gettable(L, LUA_REGISTRYINDEX);
	/* Call the generator function */
	lua_call(L, 0, 1);
	if (lua_isnil(L, -1)) {
		/* If the generator function returned nil, return NULL */
		lua_pop(L, 1);
		return NULL;
	}
	size_t len;
	/* Get the string returned by the generator function */
	const char *str = lua_tolstring(L, -1, &len);
	/* Allocate memory to copy the string */
	char *newstr = (char*) malloc(len+1);
	/* Copy the returned string */
	strncpy(newstr, str, len+1);
	/* Return the copied string */
	return newstr;
}

/* Wrapper to provide a generator function for libreadline */
static char* gen_function(const char* text, int state)
{
	/* If completion iterator wasn't started, initialize it */
	if (!state)
		lua_initgenerator(globalL, text);
	/* Call the completion iterator */
	return lua_stepgenerator(globalL);
}

/**
 * A signal handler returning back into lua_readline function
 */
static void readline_sigint(int sig)
{
	siglongjmp(globalEnv, sig);
}

/**
 * Wrapper function for the readline function
 * Args:
 * 1) Prompt - a string to display before the user input area
 * 2) Generator - a Lua function that returns an iterator of completions or a table of possible completions 
 * The generator function gets called with a single argument - the prefix of a word that has been already entered.
 * Note: this function is not reenterable as it sets libreadline global variables, Lua registry values and system signal handlers
 * Not sure if it will behave correctly in case a signal arrives while the generator function is running, and the signal handler doesn't cause the process to terminate
 */
static int lua_readline(lua_State *L)
{
	const char *prompt = lua_tolstring(L, 1, NULL);
	int type = lua_type(L, 2);
	lua_checkstack(L, 2);
	lua_pushlightuserdata(L, REGISTRY_KEY_GENERATOR);
	/* Check what type of generator we have */
	switch(type) {
		case LUA_TFUNCTION:
			/* The generator is a Lua function, use it as is */
			lua_pushvalue(L, 2);
			break;
		case LUA_TTABLE:
			/* The generator is a Lua table, create an iterator with ipairs */
			lua_pushvalue(L, 2);
			lua_pushcclosure(L, lua_ipairsiterator, 1);
			break;
		default:
			/* We have no generator, use an "empty" iterator instead of it */
			lua_pushcfunction(L, lua_niliterator);
	}
	/* Store Lua generator function to the registry */
	lua_settable(L, LUA_REGISTRYINDEX);
	/* Point libreadlint to our generator wrapper */
	rl_completion_entry_function = gen_function;
	/* Set globalL for it to be available in signal handlers and in the generator function */
	globalL = L;
	/* Save old signal handler */
	sig_t old_sigint = signal(SIGINT, readline_sigint);
	/* Trap SIGINT handler to cancel libreadline input */
	int sig = sigsetjmp(globalEnv, 1);
	if (sig) {
		/* Clean up libreadline after a signal */
		rl_free_line_state();
		rl_cleanup_after_signal();
		/* Restore the old signal handler */
		signal(SIGINT, old_sigint);
		/* Call the old signal handler if there was one */
		if (old_sigint!=SIG_DFL &&
		    old_sigint!=SIG_ERR &&
		    old_sigint!=SIG_IGN)
			old_sigint(sig);
		/* If the signal handler didn't cause the process to terminate, return nil */
		return 0;
	}
	char *line = readline(prompt);
	/* Restore signals on return */
	signal(SIGINT, old_sigint);
	if (!line) {
		/* If NULL was returned, check for EOF on input */
		FILE *fd = rl_instream;
		if (!fd)
			fd = stdin;
		if (feof(fd))
			/* If input is EOF, return nil */
			return 0;
	}
	/* Return the string read by readline() */
	lua_pushstring(L, line);
	/* Free the string returned by readline() */
	free(line);
	return 1;
}

/**
 * Wrapper function to get rl_readline_name variable
 */
static int lua_getname(lua_State *L)
{
	lua_pushstring(L, rl_readline_name);
	return 1;
}

/**
 * Wrapper function to set rl_readline_name variable
 */
static int lua_setname(lua_State *L)
{
	const char *name = lua_tolstring(L, 1, NULL);
	if (rl_readline_name)
		free((void*) rl_readline_name);
	rl_readline_name = clonestr(name);
	if (!rl_readline_name)
		return luaL_error(L, "Out of memory");
	return 0;
}

/**
 * Wrapper for readline addhistory() function
 */
static int lua_addhistory(lua_State *L)
{
	const char *str = lua_tolstring(L, 1, NULL);
	add_history(str);
	return 0;
}

typedef struct _reg_t {
	const char *name;
	int (*func)(lua_State*);
} reg_t;

/**
 * The functions provided by this library
 */
reg_t R[] = {
	{"readline", lua_readline},
	{"addhistory", lua_addhistory},
	{"getname", lua_getname},
	{"setname", lua_setname},
	{NULL, NULL},
};

/**
 * Lua 5.1 compatibility function
 * Puts a set of C functions into a table
 */
static void lua_reg(lua_State *L, reg_t *reg)
{
	reg_t *r;
	for (r=reg; r->name && r->func; r++) {
		lua_pushcfunction(L, r->func);
		lua_setfield(L, -2, r->name);
	}
}

/**
 * Creates a table representing readline library functions and returns it
 */
int luaopen_readline(lua_State *L) {
	lua_checkstack(L, 2);
	lua_createtable(L, 0, 4);
	lua_reg(L, R);
	return 1;
}


#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

void pack_table(lua_State* L, luaL_Buffer* buffer, int index, int depth);

void
pack_one(lua_State* L, luaL_Buffer* buffer,int index, int depth) {
	int type = lua_type(L, index);
	switch (type)
	{
	case LUA_TNIL:
		luaL_addstring(buffer, "nil");
		break;
	case LUA_TNUMBER:
	{
		int x = (int)lua_tointeger(L, index);
		lua_Number n = lua_tonumber(L, index);
		if ((lua_Number)x == n) 
		{
			const char* str = lua_pushfstring(L, "%d", x);
			luaL_addstring(buffer, str);
			lua_pop(L, 1);
		}
		else
		{
			const char* str = lua_pushfstring(L, "%f", x);
			luaL_addstring(buffer, str);
			lua_pop(L, 1);
		}
		break;
	}
	case LUA_TBOOLEAN:
	{
						 int val = lua_toboolean(L, index);
						 if (val)
						 {
							 const char* str = lua_pushfstring(L, "true");
							 luaL_addstring(buffer, str);
							 lua_pop(L, 1);
						 }
						 else
						 {
							 const char* str = lua_pushfstring(L, "false");
							 luaL_addstring(buffer, str);
							 lua_pop(L, 1);
						 }
						 break;
	}
	case LUA_TSTRING:
	{
		size_t sz = 0;
		const char *str = lua_tolstring(L, index, &sz);
		luaL_addstring(buffer, "\"");
		luaL_addlstring(buffer, str,sz);
		luaL_addstring(buffer, "\"");
		break;
	}
	case LUA_TTABLE:
		if (index < 0) {
			index = lua_gettop(L) + index + 1;
		}
		pack_table(L, buffer,index, ++depth);
		break;
	default:

		luaL_error(L, "unsupport type %s to serialize", lua_typename(L, type));
		break;
	}
}



void
pack_table(lua_State* L, luaL_Buffer* buffer, int index, int depth) {
	luaL_addstring(buffer, "{\n");
	int array_size = lua_rawlen(L, index);
	int top = lua_gettop(L);
	int i;
	for (i = 1; i <= array_size; i++)
	{
		lua_rawgeti(L, index, i);
		int t;
		for (t = 0; t < depth;t++)
			luaL_addchar(buffer, '\t');
		pack_one(L, buffer, -1, depth);
		luaL_addstring(buffer, ",\n");
		lua_pop(L, 1);
	}

	lua_pushnil(L);
	while (lua_next(L, index) != 0)
	{
		if (lua_type(L,-2) == LUA_TNUMBER)
		{
			int i = (int)lua_tointeger(L, -2);
			lua_Number n = lua_tonumber(L, -2);
			if ((lua_Number)i == n)
			{
				if (i > 0 && i <= array_size)
				{
					lua_pop(L, 1);
					continue;
				}
			}
		}
		int t;
		for (t = 0; t < depth; t++)
			luaL_addchar(buffer, '\t');

		luaL_addstring(buffer, "[");
		pack_one(L, buffer, -2, depth);
		luaL_addstring(buffer, "] = ");
		pack_one(L, buffer, -1, depth);

		luaL_addstring(buffer, ",\n");

		lua_pop(L, 1);
	}
	int t;
	for (t = 0; t < depth-1; t++)
		luaL_addchar(buffer, '\t');
	luaL_addstring(buffer, "}");
}

static int
serialze(lua_State* L) {
	int type = lua_type(L, 1);
	if (type != LUA_TTABLE)
		luaL_error(L, "must be table");
	
	luaL_Buffer buffer;
	luaL_buffinit(L, &buffer);

	luaL_addstring(&buffer, "return");

	pack_table(L, &buffer, 1, 1);

	luaL_pushresult(&buffer);
	return 1;
}

int main()
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	lua_pushcfunction(L, serialze);
	
	int ok = luaL_loadfile(L, "tbl.lua");
	if (ok != LUA_OK)  {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		return 0;
	}

	ok = lua_pcall(L, 0, 1, 0);
	if (ok != LUA_OK)  {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		return 0;
	}

	ok = lua_pcall(L, 1, 1, 0);
	if (ok != LUA_OK)  {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		return 0;
	}

	const char* str = lua_tostring(L, -1);
	FILE* file = fopen("test.lua", "w");
	fwrite(str, 1, strlen(str), file);
	fclose(file);
}
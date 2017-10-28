#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#define BUFFER_SIZE 64 * 1024

struct write_buffer {
	char* ptr;
	size_t size;
	size_t offset;
	char init[BUFFER_SIZE];
};

void buffer_init(struct write_buffer* buffer)
{
	buffer->ptr = buffer->init;
	buffer->size = BUFFER_SIZE;
	buffer->offset = 0;
}

void buffer_reservce(struct write_buffer* buffer, size_t len)
{
	if (buffer->offset + len > buffer->size)
	{
		size_t nsize = buffer->size * 2;
		while (nsize < buffer->offset + len)
		{
			nsize = nsize * 2;
		}
		char* nptr = (char*)malloc(nsize);
		memset(nptr, 0, nsize);
		memcpy(nptr, buffer->ptr, buffer->size);
		buffer->size = nsize;

		if (buffer->ptr != buffer->init)
			free(buffer->ptr);
		buffer->ptr = nptr;
	}
}

void buffer_addchar(struct write_buffer* buffer, char c)
{
	buffer_reservce(buffer, 1);
	buffer->ptr[buffer->offset++] = c;
}

void buffer_addstring(struct write_buffer* buffer, const char* str)
{
	int len = strlen(str);
	buffer_reservce(buffer, len);
	memcpy(buffer->ptr + buffer->offset, str, len);
	buffer->offset += len;
}

void buffer_addlstring(struct write_buffer* buffer, const char* str,size_t len)
{
	buffer_reservce(buffer, len);
	memcpy(buffer->ptr + buffer->offset, str, len);
	buffer->offset += len;
}

void buffer_release(struct write_buffer* buffer)
{
	if (buffer->ptr != buffer->init)
		free(buffer->ptr);
}

#define tab(buffer,depth) do {int i;for(i=0;i<depth;i++)buffer_addchar(buffer, '\t');}while(0)
#define newline(buffer) buffer_addstring(buffer, ",\n")



void pack_table(lua_State* L, struct write_buffer* buffer, int index, int depth);

void pack_one(lua_State* L, struct write_buffer* buffer, int index, int depth)
{
	int type = lua_type(L, index);
	switch (type)
	{
		case LUA_TNIL:
			buffer_addstring(buffer, "nil");
			break;
		case LUA_TNUMBER:
		{
			int x = (int)lua_tointeger(L, index);
			lua_Number n = lua_tonumber(L, index);
			if ((lua_Number)x == n) 
			{
				const char* str = lua_pushfstring(L, "%d", x);
				buffer_addstring(buffer, str);
				lua_pop(L, 1);
			}
			else
			{
				const char* str = lua_pushfstring(L, "%f", x);
				buffer_addstring(buffer, str);
				lua_pop(L, 1);
			}
			break;
		}
		case LUA_TBOOLEAN:
		{
			 int val = lua_toboolean(L, index);
			 if (val)
				 buffer_addstring(buffer, "true");
			 else
				 buffer_addstring(buffer, "false");
			 break;
		}
		case LUA_TSTRING:
		{
			size_t sz = 0;
			const char *str = lua_tolstring(L, index, &sz);
			buffer_addstring(buffer, "\"");
			buffer_addlstring(buffer, str, sz);
			buffer_addstring(buffer, "\"");
			break;
		}
		case LUA_TTABLE:
		{
		   if (index < 0) {
			   index = lua_gettop(L) + index + 1;
		   }
		   pack_table(L, buffer, index, ++depth);
		   break;
		}
		default:
			luaL_error(L, "unsupport type %s to serialize", lua_typename(L, type));
			break;
	}
}



void pack_table(lua_State* L, struct write_buffer* buffer, int index, int depth) {
	buffer_addstring(buffer, "{\n");
	int array_size = lua_rawlen(L, index);
	int i;
	for (i = 1; i <= array_size; i++)
	{
		lua_rawgeti(L, index, i);
		tab(buffer, depth);
		pack_one(L, buffer, -1, depth);
		newline(buffer);
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

		tab(buffer, depth);

		buffer_addstring(buffer, "[");
		pack_one(L, buffer, -2, depth);
		buffer_addstring(buffer, "] = ");

		pack_one(L, buffer, -1, depth);

		newline(buffer);

		lua_pop(L, 1);
	}
	tab(buffer, depth-1);
	buffer_addstring(buffer, "}");
}

static int serialze(lua_State* L) 
{
	int type = lua_type(L, 1);
	if (type != LUA_TTABLE)
		luaL_error(L, "must be table");
	
	struct write_buffer buffer;
	buffer_init(&buffer);
	buffer_addstring(&buffer, "return");

	pack_table(L, &buffer, 1, 1);

	lua_pushlstring(L, buffer.ptr, buffer.offset);

	buffer_release(&buffer);
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

	lua_call(L, 1, 1);


	const char* str = lua_tostring(L, -1);
	FILE* file = fopen("test.lua", "w");
	fwrite(str, 1, strlen(str), file);
	fclose(file);
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

union field_type {
	int builtin;
	struct protocol* protocol;
};

struct field {
	const char* name;
	union field_type field_type;
};

struct protocol_table;

struct protocol {
	struct protocol* next;
	struct protocol_table* nest;

	const char* name;
	struct field** field;
	int cap;
	int size;
};

struct protocol_table {
	struct protocol** slots;
	int size;
};

typedef void(*protocol_begin_func)(void* userdata, const char* name);
typedef void(*protocol_over_func)(void* userdata);
typedef void(*field_begin_func)(void* userdata,const char* field_type);
typedef void(*field_over_func)(void* userdata, const char* field_name);

struct parser {
	char* c;
	int offset;
	int size;
	int line;
	struct protocol_table* table;
	protocol_begin_func protocol_begin;
	protocol_over_func protocol_over;
	field_begin_func field_begin;
	field_begin_func field_over;
};

int strhash(const char *str)
{
	int hash = 0;
	int ch;
	for (long i = 0; ch = (int)*str++; i++)
	{
		if ((i & 1) == 0)
			hash ^= ((hash << 7) ^ ch ^ (hash >> 3));
		else
			hash ^= (~((hash << 11) ^ ch ^ (hash >> 5)));
	}
	return hash;
}

struct protocol_table* create_table(int size)
{
	struct protocol_table* table = (struct protocol_table*)malloc(sizeof(*table));
	table->size = size;

	table->slots = (struct protocol**)malloc(sizeof(*table->slots) * size);
	memset(table->slots, 0, sizeof(*table->slots) * size);

	return table;
}

struct protocol* query_protocol(struct protocol_table* table, const char* name)
{
	int hash = strhash(name);
	int index = hash % table->size;
	struct protocol* slot = table->slots[index];
	if (slot != NULL)
	{
		while (slot)
		{
			if ((memcmp(slot->name, name,strlen(name)) == 0))
				return slot;
			slot = slot->next;
		}
	}
	return NULL;
}

void rehash_table(struct protocol_table* table, int nsize)
{
	struct protocol** nslots = (struct protocol**)malloc(sizeof(*nslots) * nsize);
	memset(nslots, 0, sizeof(*nslots) * nsize);

	for (int i = 0; i < table->size; i++) 
	{
		struct protocol* ptl = table->slots[i];
		while (ptl) 
		{
			int hash = strhash(ptl->name);
			int index = hash % nsize;
			struct protocol* slot = nslots[index];
			if (slot == NULL)
				nslots[index] = ptl;
			else
			{
				ptl->next = slot;
				nslots[index] = ptl;
			}
			ptl = ptl->next;
		}
	}
	free(table->slots);
	table->slots = nslots;
	table->size = nsize;
}

void add_protocol(struct protocol_table* table, struct protocol* protocol)
{
	int hash = strhash(protocol->name);
	int index = hash % table->size;
	struct protocol* slot = table->slots[index];
	if (slot == NULL)
		table->slots[index] = protocol;
	else
	{
		protocol->next = slot;
		table->slots[index] = protocol;
		int size = 0;
		while (protocol)
		{
			size++;
			protocol = protocol->next;
		}
		if (size >= (table->size / 8))
			rehash_table(table, table->size * 2);
	}
}

struct field* query_field(struct protocol* protocol, const char* name)
{
	for (int i = 0; i < protocol->size; i++)
	{
		struct field* f = protocol->field[i];
		if (memcmp(f->name, name, strlen(name)) == 0)
			return f;
	}
	return NULL;
}

void add_field(struct protocol* protocol, struct field* f)
{
	if (protocol->size == protocol->cap)
	{
		int ncap = protocol->cap * 2;
		struct field** nptr = (struct field**)malloc(sizeof(*nptr) * ncap);
		memset(nptr, 0, sizeof(*nptr) * ncap);
		memcpy(nptr, protocol->field, sizeof(*nptr) * protocol->cap);
		free(protocol->field);
		protocol->field = nptr;
		protocol->cap = ncap;
	}
	protocol->field[protocol->size++] = f;
}

struct protocol* create_protocol(const char* name)
{
	struct protocol* ctx = (struct protocol*)malloc(sizeof(*ctx));
	ctx->next = NULL;
	ctx->name = name;
	ctx->nest = create_table(4);
	ctx->cap = 4;
	ctx->size = 0;
	ctx->field = (struct field**)malloc(sizeof(struct field*) * ctx->cap);
	memset(ctx->field, 0, sizeof(struct field*) * ctx->cap);

	return ctx;
}

void parser_init(struct parser* parser, const char* file,void* ud, protocol_begin_func ptl_begin, protocol_over_func ptl_over, field_begin_func field_begin, field_over_func field_over)
{
	FILE *file_handle = fopen(file, "r");
	fseek(file_handle, 0, SEEK_END);
	int len = ftell(file_handle);
	parser->offset = 0;
	parser->size = len;
	parser->line = 1;
	parser->c = (char*)malloc(len+1);
	memset(parser->c, 0, len + 1);
	rewind(file_handle);
	fread(parser->c, 1, len, file_handle);
	parser->c[len] = 0;
	fclose(file_handle);

	parser->table = create_table(16);
	parser->protocol_begin = ptl_begin;
	parser->protocol_over = ptl_over;
	parser->field_begin = field_begin;
	parser->field_over = field_over;
}

static int eos(struct parser *p)
{
	if (*(p->c) == 0)
		return 1;
	else
		return 0;
}

static void skip_space(struct parser *p);

static void next_line(struct parser *p)
{
	char *n = p->c;
	while (*n != '\n' && *n)
		n++;
	if (*n == '\n')
		n++;
	p->line++;
	p->c = n;
	skip_space(p);

	return;
}

static void skip_space(struct parser *p)
{
	char *n = p->c;
	while (isspace(*n) && *n) {
		if (*n == '\n')
			p->line++;
		n++;
	}

	p->c = n;
	if (*n == '#' && *n)
		next_line(p);

	return;
}

static void next_token(struct parser *p)
{
	char *n;
	skip_space(p);
	n = p->c;
	while (!isspace(*n) && *n)
		n++;
	p->c = n;
	skip_space(p);

	return;
}

static void skip(struct parser* p, int size)
{
	char *n = p->c;
	int index = 0;
	while (!eos(p) && index < size)
	{
		n++;
		index++;
	}
		
	p->c = n;
}

static bool expect(struct parser* p,int offset,char c)
{
	return *(p->c + offset) == c;
}

static bool expect_space(struct parser* p, int offset)
{
	return isspace(*(p->c + offset));
}



static const char* builtin_type[] = { "int","number", "string", "protocol"};

void parser_run(struct parser* p)
{
	skip_space(p);
	char name[65];
	memset(name, 0, 65);
	char* c = p->c;
	int err = sscanf(p->c, "%64[1-9a-zA-Z]", name);
	if (err == 0) {
		fprintf(stderr, "line:%d syntax error", p->line);
		return;
	}

	int name_length = strlen(name);

	if (memcmp(name, "protocol", name_length) == 0)
	{
		if (isspace(*(p->c + name_length)) == 0)
		{
			fprintf(stderr, "line:%d syntax error", p->line);
			return;
		}
		next_token(p);
		err = sscanf(p->c, "%64[1-9a-zA-Z]", name);
		p->protocol_begin(p->table, name);

		name_length = strlen(name);
		if (!expect_space(p, name_length) && !expect(p,name_length,'{'))
		{
			fprintf(stderr, "line:%d syntax error", p->line);
			return;
		}
		skip(p, name_length);
	}
	else
	{
		fprintf(stderr, "line:%d syntax error", p->line);
		return;
	}

	skip_space(p);
	if (!expect(p, 0, '{'))
	{
		fprintf(stderr, "line:%d syntax error", p->line);
		return;
	}
	while (!eos(p))
	{
		next_token(p);
		if (expect(p, 0, '}'))
		{
			next_token(p);
			p->protocol_over(p->table);
			break;
		}
		memset(name, 0, 65);
		err = sscanf(p->c, "%64s", name);
		if (err == 0)
		{
			fprintf(stderr, "line:%d syntax error", p->line);
			return;
		}
		int len = strlen(name);
		bool success = false;
		for (int i = 0; i < sizeof(builtin_type) / sizeof(void*); i++)
		{
			if (memcmp(name, builtin_type[i], len) == 0)
			{
				success = true;
				break;
			}
		}
		if (!success)
		{
			fprintf(stderr, "line:%d syntax error:unknown type:%s\n", p->line, name);
			return;
		}

		if (expect_space(p,len) == 0)
		{
			fprintf(stderr, "line:%d syntax error", p->line);
			return;
		}

		if (memcmp(name, "protocol", len) == 0)
		{
			parser_run(p);
			continue;
		}
		else
		{
			p->field_begin(p->table, name);
		}

		next_token(p);
		memset(name, 0, 65);
		err = sscanf(p->c, "%64s", name);
		if (err == 0)
		{
			fprintf(stderr, "line:%d syntax error", p->line);
			return;
		}
		len = strlen(name);
		p->field_over(p->table, name);
	}
	
}

void protobol_begin(void* userdata, const char* name)
{
	printf("protobol_begin:%s\n", name);
}

void protobol_over(void* userdata)
{
	printf("protobol_over\n");
}

void field_begin(void* userdata, const char* field_type)
{
	printf("field_begin:%s\n", field_type);
}

void field_over(void* userdata, const char* field_name)
{
	printf("field_over:%s\n", field_name);
}


int main()
{
	struct parser p;
	parser_init(&p, "test.protocol", NULL, protobol_begin, protobol_over, field_begin, field_over);
	while (!eos(&p))
	{
		parser_run(&p);
	}
	
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <setjmp.h>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#define TRY(l) if (setjmp((l)->exception) == 0)
#define THROW(l) longjmp((l)->exception, 1)

#define TYPE_INT				0
#define TYPE_INT_ARRAY			1
#define TYPE_FLOAT				2
#define TYPE_FLOAT_ARRAY		3
#define TYPE_DOUBLE				4
#define TYPE_DOUBLE_ARRAY		5
#define TYPE_STRING				6
#define TYPE_STRING_ARRAY		7
#define TYPE_PROTOCOL			8
#define TYPE_PROTOCOL_ARRAY		9

static const char* builtin_type[] = { "int", "int[]", "float", "float[]", "double", "double[]", "string", "string[]", "protocol", "protocol" };

struct field_type {
	int type;
	struct protocol* protocol;
};

struct field {
	char* name;
	struct field_type field_type;
};

struct protocol_table;

struct protocol {
	struct protocol* next;
	struct protocol* parent;
	struct protocol_table* children;

	char* name;
	struct field** field;
	int cap;
	int size;

	char* lastfield;
};

struct protocol_table {
	struct protocol** slots;
	int size;
};

typedef struct protocol* (*protocol_begin_func)(struct protocol_table* table, const char* name);
typedef void(*protocol_over_func)(struct protocol_table* table);
typedef void(*field_begin_func)(struct protocol* ptl,const char* field_type);
typedef void(*field_over_func)(struct protocol* ptl, const char* field_name);


struct file_hash {
	char** name;
	int offset;
	int size;
};

struct lexer {
	char* c;
	int line;
	char* file;

	struct lexer* main;

	jmp_buf exception;
	struct protocol* root;

	struct file_hash file_hash;

	protocol_begin_func protocol_begin;
	protocol_over_func protocol_over;
	field_begin_func field_begin;
	field_begin_func field_over;
};


size_t strhash(const char *str)
{
	size_t hash = 0;
	int ch;
	for (long i = 0; ch = (size_t)*str++; i++)
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
	size_t hash = strhash(name);
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
			size_t hash = strhash(ptl->name);
			int index = hash % nsize;
			struct protocol* slot = nslots[index];
			if (slot == NULL)
			{
				struct protocol* tmp = ptl->next;
				ptl->next = NULL;
				nslots[index] = ptl;
				ptl = tmp;
			}
			else
			{
				struct protocol* tmp = ptl->next;
				ptl->next = slot;
				nslots[index] = ptl;
				ptl = tmp;
				continue;
			}
			
		}
	}
	free(table->slots);
	table->slots = nslots;
	table->size = nsize;
}

void add_protocol(struct protocol_table* table, struct protocol* protocol)
{
	size_t hash = strhash(protocol->name);
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

struct field* create_field(struct protocol* ptl,char* field_type, char* field_name)
{
	int ftype = TYPE_PROTOCOL;
	for (int i = 0; i < sizeof(builtin_type) / sizeof(void*); i++)
	{
		if (memcmp(field_type, builtin_type[i], strlen(field_type)) == 0)
		{
			ftype = i;
			break;
		}
	}

	struct field* f = (struct field*)malloc(sizeof(*f));
	memset(f, 0, sizeof(*f));
	f->name = field_name;
	f->field_type.type = ftype;
	f->field_type.protocol = NULL;

	if (ftype == TYPE_PROTOCOL)
	{	
		struct protocol* cursor = ptl;
		while (cursor)
		{
			struct protocol* tmp = query_protocol(cursor->children,field_type);
			if (tmp)
			{
				f->field_type.protocol = tmp;
				break;
			}
			cursor = cursor->parent;
		}
		assert(f->field_type.protocol != NULL);
	}

	return f;
}

struct protocol* create_protocol(const char* name)
{
	int len = strlen(name);
	struct protocol* ctx = (struct protocol*)malloc(sizeof(*ctx));
	ctx->next = NULL;
	ctx->name = (char*)malloc(len + 1);
	memcpy((void*)ctx->name, (void*)name, len);
	ctx->name[len] = '\0';
	ctx->parent = NULL;
	ctx->children = create_table(4);
	ctx->cap = 4;
	ctx->size = 0;
	ctx->field = (struct field**)malloc(sizeof(struct field*) * ctx->cap);
	memset(ctx->field, 0, sizeof(struct field*) * ctx->cap);

	return ctx;
}

void dump_protocol(struct protocol* root,int depth)
{
	for (int i = 0; i < depth; ++i)
		printf("\t");

	printf("protocol:%s\n",root->name);
	depth++;
	struct protocol_table* table = root->children;
	for (int i = 0; i < table->size; i++) 
	{
		struct protocol* ptl = table->slots[i];
		while (ptl) 
		{
			dump_protocol(ptl,depth);
			ptl = ptl->next;
		}
	}

	for (int i = 0; i < root->size; ++i)
	{
		struct field* f = root->field[i];
		for (int i = 0; i < depth; ++i)
			printf("\t");

		printf("field type:%d,",f->field_type.type);
		if (f->field_type.type == TYPE_PROTOCOL || f->field_type.type == TYPE_PROTOCOL_ARRAY) {
			printf("type name:%s,",f->field_type.protocol->name);
		} else {
			printf("type name:%s,",builtin_type[f->field_type.type]);
		}
		printf("field name:%s\n",f->name);
	}
}

void lexer_init(struct lexer* l, struct protocol* root, protocol_begin_func ptl_begin, protocol_over_func ptl_over, field_begin_func field_begin, field_over_func field_over)
{
	if (root)
		l->root = root;
	else
		l->root = create_protocol("root");
	l->file_hash.offset = 0;
	l->file_hash.size = 16;
	l->file_hash.name = (char**)malloc(sizeof(char*)* l->file_hash.size);
	memset(l->file_hash.name, 0, sizeof(char*)* l->file_hash.size);

	l->protocol_begin = ptl_begin;
	l->protocol_over = ptl_over;
	l->field_begin = field_begin;
	l->field_over = field_over;
}

bool exist_file(struct lexer* l, char* file)
{
	int i;
	for (i = 0; i < l->file_hash.offset;i++)
	{
		char* tmp = l->file_hash.name[i];
		if (memcmp(tmp,file,strlen(file)) == 0)
			return true;
	}
	return false;
}

void parse_done(struct lexer* l, char* file)
{
	if (l->file_hash.offset == l->file_hash.size)
	{
		int nsize = l->file_hash.size;
		char** nptr = (char**)malloc(sizeof(char*)*nsize);
		memset(nptr, 0, sizeof(char*)*nsize);
		memcpy(nptr, l->file_hash.name, l->file_hash.size*sizeof(char*));
		free(l->file_hash.name);
		l->file_hash.size = nsize;
		l->file_hash.name = nptr;
	}
	char* tmp = (char*)malloc(strlen(file) + 1);
	memcpy(tmp, file, strlen(file));
	tmp[strlen(file)] = '\0';
	l->file_hash.name[l->file_hash.offset++] = tmp;
}

static int eos(struct lexer *l, int n)
{
	if (*(l->c + n) == 0)
		return 1;
	else
		return 0;
}

static void skip_space(struct lexer *l);

static void next_line(struct lexer *l)
{
	char *n = l->c;
	while (*n != '\n' && *n)
		n++;
	if (*n == '\n')
		n++;
	l->line++;
	l->c = n;
	skip_space(l);

	return;
}

static void skip_space(struct lexer *l)
{
	char *n = l->c;
	while (isspace(*n) && *n) {
		if (*n == '\n')
			l->line++;
		n++;
	}

	l->c = n;
	if (*n == '#' && *n)
		next_line(l);

	return;
}

static void next_token(struct lexer *l)
{
	char *n;
	skip_space(l);
	n = l->c;
	while (!isspace(*n) && *n)
		n++;
	l->c = n;
	skip_space(l);

	return;
}

static void skip(struct lexer* l, int size)
{
	char *n = l->c;
	int index = 0;
	while (!eos(l,0) && index < size)
	{
		n++;
		index++;
	}
		
	l->c = n;
}

static bool expect(struct lexer* l, int offset, char c)
{
	return *(l->c + offset) == c;
}

static bool expect_space(struct lexer* l, int offset)
{
	return isspace(*(l->c + offset));
}

void lexer_parse(struct lexer* l, struct protocol* parent);

int lexer_parse_file(struct lexer* l, const char* file)
{
	FILE *file_handle = fopen(file, "r");
	fseek(file_handle, 0, SEEK_END);
	int len = ftell(file_handle);
	l->c = (char*)malloc(len + 1);
	memset(l->c, 0, len + 1);
	rewind(file_handle);
	fread(l->c, 1, len, file_handle);
	l->c[len] = 0;
	fclose(file_handle);

	l->line = 1;
	l->file = (char*)malloc(strlen(file) + 1);
	memcpy(l->file, file, strlen(file));
	l->file[strlen(file)] = '\0';

	TRY(l) {
		while (!eos(l, 0))
		{
			lexer_parse(l, l->root);
		}
		parse_done(l->main, (char*)file);
		return 0;
	}
	return -1;
}

struct protocol* protobol_begin(struct protocol_table* table, const char* name)
{
	printf("protobol_begin:%s\n", name);
	struct protocol* ptl = create_protocol(name);
	add_protocol(table, ptl);
	return ptl;
}

void protobol_over(struct protocol_table* table)
{
	printf("protobol_over\n");
}

void field_begin(struct protocol* ptl, const char* field_type)
{
	printf("field_begin:%s\n", field_type);
	int len = strlen(field_type);
	ptl->lastfield = (char*)malloc(len + 1);
	memcpy(ptl->lastfield, field_type, len);
	ptl->lastfield[len] = '\0';
}

void field_over(struct protocol* ptl, const char* field_name)
{
	printf("field_over:%s\n", field_name);
	int len = strlen(field_name);
	char* fname = (char*)malloc(len + 1);
	memcpy(fname, field_name, len);
	fname[len] = '\0';

	struct field* f = create_field(ptl, ptl->lastfield, fname);
	add_field(ptl, f);

	ptl->lastfield = NULL;
}

void lexer_parse(struct lexer* l, struct protocol* parent);

void import_protocol(struct lexer* l,char* name)
{
	char file[64];
	memset(file, 0, 64);
	sprintf(file, "%s.protocol", name);

	struct lexer import_lexer;
	lexer_init(&import_lexer, l->main->root, protobol_begin, protobol_over, field_begin, field_over);
	import_lexer.main = l->main;
	if (lexer_parse_file(&import_lexer, file) < 0)
	{
		THROW(l);
	}
}

void parse_protocol(struct lexer* l, struct protocol* parent)
{
	char name[65];
	memset(name, 0, 65);

	next_token(l);

	int err = sscanf(l->c, "%64[1-9a-zA-Z_]", name);
	int len = strlen(name);

	//协议名不能超过64
	if (!expect_space(l, len) && !expect(l, len, '{'))
	{
		fprintf(stderr, "file:%s@line:%d syntax error:protocol name:%s too long", l->file, l->line, name);
		THROW(l);
	}

	struct protocol* optl = query_protocol(parent->children,name);
	if (optl) {
		fprintf(stderr, "file:%s@line:%d syntax error:protocol name:%s already define\n", l->file, l->line, name);
		THROW(l);
	}

	struct protocol* ptl = l->protocol_begin(parent->children, name);
	ptl->parent = parent;

	//跳过协议名
	skip(l, len);

	//跳过空格
	skip_space(l);

	//协议名后有>=0个空格，空格之后必须是{
	if (!expect(l, 0, '{'))
	{
		fprintf(stderr, "file:%s@line:%d syntax error", l->file, l->line);
		THROW(l);
	}

	//{之后必有空格
	if (!expect_space(l, 1))
	{
		fprintf(stderr, "file:%s@line:%d syntax error:expect space", l->file, l->line);
		THROW(l);
	}
	while (!eos(l,0))
	{
		next_token(l);

	__again:
		if (expect(l, 0, '}'))
		{
			if (!eos(l,1) && !expect_space(l, 1))
			{
				fprintf(stderr, "file:%s@line:%d syntax error", l->file, l->line);
				THROW(l);
			}
			l->protocol_over(parent->children);
			next_token(l);
			break;
		}
		memset(name, 0, 65);
		err = sscanf(l->c, "%64s", name);
		if (err == 0)
		{
			fprintf(stderr, "file:%s@line:%d syntax error", l->file, l->line);
			THROW(l);
		}
		int len = strlen(name);

		if (memcmp(name, "protocol", len) == 0)
		{
			parse_protocol(l, ptl);
			if (expect_space(l, 0))
				continue;
			else
				goto __again;
		}

		bool builtin = false;
		for (int i = 0; i < sizeof(builtin_type) / sizeof(void*); i++)
		{
			if (memcmp(name, builtin_type[i], len) == 0)
			{
				builtin = true;
				break;
			}
		}
		if (!builtin)
		{
			struct protocol* protocol = query_protocol(parent->children, name);
			if (protocol == NULL)
			{
				fprintf(stderr, "file:%s@line:%d syntax error:unknown type:%s\n", l->file, l->line, name);
				THROW(l);
			}			
		}

		if (expect_space(l,len) == 0)
		{
			fprintf(stderr, "file:%s@line:%d syntax error", l->file, l->line);
			THROW(l);
		}

		l->field_begin(ptl, name);
		
		next_token(l);
		memset(name, 0, 65);
		err = sscanf(l->c, "%64[1-9a-zA-Z_]", name);
		if (err == 0)
		{
			fprintf(stderr, "file:%s@line:%d syntax error", l->file, l->line);
			THROW(l);
		}
		len = strlen(name);
		l->field_over(ptl, name);
		
		//每一个字段名之后，必有空格
		if (!expect_space(l, len))
		{
			fprintf(stderr, "file:%s@line:%d syntax error:expect space",l->file, l->line);
			THROW(l);
		}
	}
}

void lexer_parse(struct lexer* l, struct protocol* parent)
{
	skip_space(l);
	char name[65];
	memset(name, 0, 65);

	int err = sscanf(l->c, "%64[1-9a-zA-Z]", name);
	if (err == 0) {
		fprintf(stderr, "file:%s@line:%d syntax error", l->file, l->line);
		THROW(l);
	}

	int len = strlen(name);
	if (memcmp(name, "protocol", len) == 0)
	{
		return parse_protocol(l, parent);
	
	}
	else if (memcmp(name, "import", len) == 0)
	{
		if (!expect_space(l,len))
		{
			fprintf(stderr, "file:%s@line:%d syntax error:expect space\n", l->file, l->line);
			THROW(l);
		}
		next_token(l);
		if (!expect(l,0,'\"'))
		{
			fprintf(stderr, "file:%s@line:%d syntax error:expect \"\n", l->file, l->line);
			THROW(l);
		}
		err = sscanf(l->c, "\"%64[^\"]\"", name);
		if (err == 0) {
			fprintf(stderr, "file:%s@line:%d syntax error", l->file, l->line);
			THROW(l);
		}

		if (exist_file(l->main, name) == false)
			import_protocol(l, name);
		
		
		skip(l, strlen(name) + 2);
		if (!expect_space(l, 0))
		{
			fprintf(stderr, "file:%s@line:%d syntax error:expect space\n",l->file, l->line);
			THROW(l);
		}
		return;
	}
	fprintf(stderr, "file:%s@line:%d syntax error:unknown %s", l->file, l->line, name);
	THROW(l);
}


int main()
{
	struct lexer l;
	lexer_init(&l, NULL, protobol_begin, protobol_over, field_begin, field_over);
	l.main = &l;
	lexer_parse_file(&l, "test.protocol");
	dump_protocol(l.root,0);
}
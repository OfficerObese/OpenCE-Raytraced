/*
XGPU_HLSL.C

HLSL from the NV2A translators' GLSL (xgpu_hlsl.h).

The GLSL is a known, small part of the language: declarations at the top
(uniforms, the attributes, the varyings, the outputs) and functions that use
vectors, swizzles, a few built-in functions and texture lookups. The
declarations are read and dropped, and the HLSL declares each instead: the
uniforms in constant buffers (by the same names, so that the functions read
them unchanged), the textures as a texture and a sampler each, and the
attributes, varyings and outputs as static variables, which an entry point
fills from its inputs, then calls the GLSL's main, then returns from. The
functions are copied token by token, rewriting:
- the vector types (vec4 to float4) and constructors, a one-argument
  constructor becoming a cast (GLSL's vec4(x) repeats a scalar; HLSL's cast
  does, its constructor does not);
- the built-in functions HLSL names otherwise (fract, inversesqrt, mix, the
  vector comparisons);
- texture lookups, into the texture's and sampler's methods;
- names GLSL allows that HLSL reserves.
Anything else the GLSL might hold is refused, with the reason, so that a
change to the translators shows here rather than as a wrong picture.
*/

#include "xgpu_hlsl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- text */

struct hlsl_text
{
	char *buffer;
	size_t length, capacity;
	int failed;
};

static void text_append_length(struct hlsl_text *text, const char *data, size_t length)
{
	if (text->failed)
		return;
	if (text->length + length + 1 > text->capacity)
	{
		size_t capacity = text->capacity ? text->capacity : 4096;
		char *buffer;

		while (text->length + length + 1 > capacity)
			capacity *= 2;
		buffer = realloc(text->buffer, capacity);
		if (!buffer)
		{
			text->failed = 1;
			return;
		}
		text->buffer = buffer;
		text->capacity = capacity;
	}
	memcpy(text->buffer + text->length, data, length);
	text->length += length;
	text->buffer[text->length] = '\0';
}

static void text_append(struct hlsl_text *text, const char *format, ...)
{
	char stack[1024];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(stack, sizeof(stack), format, arguments);
	va_end(arguments);
	if (length < 0)
	{
		text->failed = 1;
		return;
	}
	if ((size_t)length < sizeof(stack))
	{
		text_append_length(text, stack, (size_t)length);
		return;
	}
	{
		char *heap = malloc((size_t)length + 1);

		if (!heap)
		{
			text->failed = 1;
			return;
		}
		va_start(arguments, format);
		vsnprintf(heap, (size_t)length + 1, format, arguments);
		va_end(arguments);
		text_append_length(text, heap, (size_t)length);
		free(heap);
	}
}

/* ---------- what the GLSL declares */

enum
{
	_texture_none = 0,
	_texture_2d,
	_texture_3d,
	_texture_cube,
};

/* the varyings, in the order of the HLSL's struct */
static const struct
{
	const char *name;
	const char *type;
	const char *semantic;
} varyings[] =
{
	{ "xD0", "float4", "COLOR0" },
	{ "xD1", "float4", "COLOR1" },
	{ "xB0", "float4", "TEXCOORD8" },
	{ "xB1", "float4", "TEXCOORD9" },
	{ "xT0", "float4", "TEXCOORD0" },
	{ "xT1", "float4", "TEXCOORD1" },
	{ "xT2", "float4", "TEXCOORD2" },
	{ "xT3", "float4", "TEXCOORD3" },
	{ "xFog", "float", "FOG" },
	{ "xWorldNormal", "float4", "TEXCOORD10" },
	{ "xWorldPosition", "float3", "TEXCOORD11" },
};

#define VARYING_COUNT ((int)(sizeof(varyings) / sizeof(varyings[0])))

/* the uniforms the constant buffers hold (xgpu_hlsl.h), as the GLSL names them */
static const char *const known_uniforms[] =
{
	"c", "viewport_scale", "viewport_offset", "point_size", "screen_offset",
	"ps_c0", "ps_c1", "ps_final_c0", "ps_final_c1", "fog_color", "fog_parameters", "alpha_reference",
	"bump_matrix", "bump_luminance", "texture_scale", "texture_lod_bias", "model_lights",
};

struct declarations
{
	int pixel;
	/* vertex shaders: each attribute's, 0 none, 1 vec4, 2 uint (packed) */
	unsigned char attributes[XGPU_HLSL_ATTRIBUTE_COUNT];
	unsigned char textures[4];
	unsigned char varying_used[VARYING_COUNT];
	int fragment_color;
};

static int fail(char *error, size_t error_size, const char *format, ...)
{
	va_list arguments;

	if (error && error_size)
	{
		va_start(arguments, format);
		vsnprintf(error, error_size, format, arguments);
		va_end(arguments);
	}
	return 0;
}

static int varying_index(const char *name)
{
	int index;

	for (index = 0; index < VARYING_COUNT; index++)
	{
		if (!strcmp(varyings[index].name, name))
			return index;
	}
	return -1;
}

/* the words of a declaration line, up to its semicolon: e.g. "uniform",
"vec4", "c", "192" for "uniform vec4 c[192];" */
static int declaration_words(const char *line, char words[6][64])
{
	int count = 0;

	while (*line && *line != ';' && count < 6)
	{
		size_t length = 0;

		while (*line && !isalnum((unsigned char)*line) && *line != '_' && *line != ';')
			line++;
		while ((isalnum((unsigned char)*line) || *line == '_') && length < 63)
			words[count][length++] = *line++;
		if (!length)
			break;
		words[count][length] = '\0';
		count++;
	}
	return count;
}

static int starts_with(const char *text, const char *prefix)
{
	return !strncmp(text, prefix, strlen(prefix));
}

/* one declaration line at the top of the GLSL; FALSE (with the reason) if it
is one the conversion does not know */
static int read_declaration(const char *line, struct declarations *declarations, char *error, size_t error_size)
{
	char words[6][64];
	int count;
	int index;

	if (starts_with(line, "#version") || starts_with(line, "precision") || starts_with(line, "invariant"))
		return 1;
	count = declaration_words(line, words);
	if (starts_with(line, "uniform"))
	{
		if (count < 3)
			return fail(error, error_size, "a uniform without a name: %s", line);
		if (!strcmp(words[1], "sampler2D") || !strcmp(words[1], "sampler3D") || !strcmp(words[1], "samplerCube"))
		{
			int stage = words[2][3] - '0';

			if (strncmp(words[2], "tex", 3) || stage < 0 || stage > 3 || words[2][4])
				return fail(error, error_size, "a sampler that is not tex0 to tex3: %s", line);
			declarations->textures[stage] = !strcmp(words[1], "sampler2D") ? _texture_2d :
				!strcmp(words[1], "sampler3D") ? _texture_3d : _texture_cube;
			return 1;
		}
		for (index = 0; index < (int)(sizeof(known_uniforms) / sizeof(known_uniforms[0])); index++)
		{
			if (!strcmp(words[2], known_uniforms[index]))
				return 1;
		}
		return fail(error, error_size, "an unknown uniform: %s", line);
	}
	/* layout(location = N) in vec4 vN_in; in uint vN_packed;
	layout(location = 0) out vec4 fragment_color; */
	if (starts_with(line, "layout"))
	{
		if (count == 6 && !strcmp(words[0], "layout") && !strcmp(words[1], "location") && !strcmp(words[3], "in"))
		{
			int attribute = atoi(words[2]);

			if (attribute < 0 || attribute >= XGPU_HLSL_ATTRIBUTE_COUNT || declarations->pixel)
				return fail(error, error_size, "an attribute out of range: %s", line);
			if (!strcmp(words[4], "vec4"))
				declarations->attributes[attribute] = 1;
			else if (!strcmp(words[4], "uint"))
				declarations->attributes[attribute] = 2;
			else
				return fail(error, error_size, "an attribute of an unknown type: %s", line);
			return 1;
		}
		if (count == 6 && !strcmp(words[3], "out") && !strcmp(words[5], "fragment_color") && declarations->pixel)
		{
			declarations->fragment_color = 1;
			return 1;
		}
		return fail(error, error_size, "an unknown layout: %s", line);
	}
	if ((starts_with(line, "in ") || starts_with(line, "out ")) && count == 3)
	{
		int varying = varying_index(words[2]);

		if (varying < 0)
			return fail(error, error_size, "an unknown varying: %s", line);
		if ((words[0][0] == 'i') != (declarations->pixel != 0))
			return fail(error, error_size, "a varying going the wrong way: %s", line);
		declarations->varying_used[varying] = 1;
		return 1;
	}
	return fail(error, error_size, "an unknown declaration: %s", line);
}

/* ---------- the functions, token by token */

/* names GLSL allows and HLSL reserves (the translators' locals) */
static const char *const reserved_names[] =
{
	"sample", "point", "line", "triangle", "lineadj", "triangleadj", "linear", "centroid", "precise",
	"shared", "groupshared", "matrix", "vector", "string", "pass", "technique", "compile", "half",
	"double", "texture", "sampler", "register", "packoffset", "snorm", "unorm", "nointerpolation",
	"noperspective", "column_major", "row_major", "export", "extern", "cbuffer", "tbuffer", "typedef",
	"interface", "class", "namespace", "sizeof", "template", "dword", "min16float", "inline", "static",
	"globallycoherent", "pixelshader", "vertexshader", "stateblock", "unsigned", "volatile",
};

/* GLSL's names for what HLSL names otherwise (not followed by arguments that
need rewriting) */
static const char *renamed(const char *name)
{
	static const char *const names[][2] =
	{
		{ "vec2", "float2" }, { "vec3", "float3" }, { "vec4", "float4" },
		{ "ivec2", "int2" }, { "ivec3", "int3" }, { "ivec4", "int4" },
		{ "uvec2", "uint2" }, { "uvec3", "uint3" }, { "uvec4", "uint4" },
		{ "bvec2", "bool2" }, { "bvec3", "bool3" }, { "bvec4", "bool4" },
		{ "fract", "frac" }, { "inversesqrt", "rsqrt" }, { "mix", "lerp" },
		{ "dFdx", "ddx" }, { "dFdy", "ddy" },
		{ "gl_Position", "xgpu_position" }, { "gl_PointSize", "xgpu_point_size" },
		{ "gl_SampleMask", "xgpu_sample_mask" },
	};
	size_t index;

	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
	{
		if (!strcmp(name, names[index][0]))
			return names[index][1];
	}
	for (index = 0; index < sizeof(reserved_names) / sizeof(reserved_names[0]); index++)
	{
		if (!strcmp(name, reserved_names[index]))
			return NULL;
	}
	return name;
}

/* GLSL built-ins the conversion does not know: refused rather than copied */
static int unknown_builtin(const char *name)
{
	static const char *const names[] =
	{
		"mod", "textureLod", "textureProj", "textureGrad", "textureOffset", "texelFetchOffset",
		"textureQueryLod", "atomicCounterIncrement", "imageLoad", "imageStore", "gl_FragCoord",
		"gl_FrontFacing", "gl_VertexID", "gl_InstanceID", "gl_FragDepth", "matrixCompMult", "outerProduct",
		"transpose", "inverse", "determinant", "bitfieldExtract", "packUnorm4x8", "unpackUnorm4x8",
		"floatBitsToInt", "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat", "any", "all", "not",
		"mat2", "mat3", "mat4",
	};
	size_t index;

	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
	{
		if (!strcmp(name, names[index]))
			return 1;
	}
	return 0;
}

struct converter
{
	const char *source;
	const struct declarations *declarations;
	char *error;
	size_t error_size;
	int failed;
};

static int is_identifier_start(char c)
{
	return isalpha((unsigned char)c) || c == '_';
}

static int is_identifier_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

/* the end of a comment starting at position, or position if none starts there */
static size_t comment_end(const char *source, size_t position, size_t end)
{
	if (position + 1 < end && source[position] == '/' && source[position + 1] == '*')
	{
		const char *close = strstr(source + position + 2, "*/");

		return close && (size_t)(close - source) + 2 <= end ? (size_t)(close - source) + 2 : end;
	}
	if (position + 1 < end && source[position] == '/' && source[position + 1] == '/')
	{
		while (position < end && source[position] != '\n')
			position++;
	}
	return position;
}

/* the position of the parenthesis that closes the one at open, and of each
top-level comma between them (at most 8 arguments); end if unbalanced */
static size_t arguments_split(const char *source, size_t open, size_t end, size_t *commas, int *comma_count)
{
	size_t position = open + 1;
	int depth = 0;

	*comma_count = 0;
	while (position < end)
	{
		size_t skipped = comment_end(source, position, end);
		char c;

		if (skipped != position)
		{
			position = skipped;
			continue;
		}
		c = source[position];
		if (c == '(' || c == '[')
		{
			depth++;
		}
		else if (c == ')' || c == ']')
		{
			if (!depth && c == ')')
				return position;
			depth--;
		}
		else if (c == ',' && !depth)
		{
			if (*comma_count >= 8)
				return end;
			commas[(*comma_count)++] = position;
		}
		position++;
	}
	return end;
}

static void convert_range(struct converter *converter, size_t begin, size_t end, struct hlsl_text *out);

/* the converted text of argument index of a call (its bounds from
arguments_split), trimmed */
static void convert_argument(struct converter *converter, size_t open, size_t close, const size_t *commas,
	int comma_count, int index, struct hlsl_text *out)
{
	size_t first = index ? commas[index - 1] + 1 : open + 1;
	size_t last = index < comma_count ? commas[index] : close;

	while (first < last && isspace((unsigned char)converter->source[first]))
		first++;
	while (last > first && isspace((unsigned char)converter->source[last - 1]))
		last--;
	convert_range(converter, first, last, out);
}

static int converter_fail(struct converter *converter, const char *format, ...)
{
	va_list arguments;

	if (!converter->failed && converter->error && converter->error_size)
	{
		va_start(arguments, format);
		vsnprintf(converter->error, converter->error_size, format, arguments);
		va_end(arguments);
	}
	converter->failed = 1;
	return 0;
}

/* the texture stage a sampler argument names (tex0 to tex3), or -1 */
static int texture_argument(struct converter *converter, size_t open, size_t close, const size_t *commas,
	int comma_count)
{
	struct hlsl_text name = { 0 };
	int stage = -1;

	convert_argument(converter, open, close, commas, comma_count, 0, &name);
	if (name.buffer && !strncmp(name.buffer, "tex", 3) && name.buffer[3] >= '0' && name.buffer[3] <= '3' &&
		!name.buffer[4] && converter->declarations->textures[name.buffer[3] - '0'])
	{
		stage = name.buffer[3] - '0';
	}
	free(name.buffer);
	if (stage < 0)
		converter_fail(converter, "a texture lookup on something other than a declared sampler");
	return stage;
}

/* a call whose name the conversion rewrites, at name_begin, its arguments'
parenthesis at open; returns the position after it, or 0 if it is not one */
static size_t convert_call(struct converter *converter, const char *name, size_t open, size_t end, struct hlsl_text *out)
{
	static const struct
	{
		const char *glsl;
		const char *hlsl;
	} vector_types[] =
	{
		{ "vec2", "float2" }, { "vec3", "float3" }, { "vec4", "float4" },
		{ "ivec2", "int2" }, { "ivec3", "int3" }, { "ivec4", "int4" },
		{ "uvec2", "uint2" }, { "uvec3", "uint3" }, { "uvec4", "uint4" },
		{ "bvec2", "bool2" }, { "bvec3", "bool3" }, { "bvec4", "bool4" },
		{ "float", "float" }, { "int", "int" }, { "uint", "uint" }, { "bool", "bool" },
	};
	static const struct
	{
		const char *glsl;
		const char *operator;
	} comparisons[] =
	{
		{ "lessThan", "<" }, { "lessThanEqual", "<=" }, { "greaterThan", ">" }, { "greaterThanEqual", ">=" },
		{ "equal", "==" }, { "notEqual", "!=" },
	};
	size_t commas[8], close;
	int comma_count, index;

	close = arguments_split(converter->source, open, end, commas, &comma_count);
	if (close >= end)
		return converter_fail(converter, "unbalanced parentheses after %s", name), 0;

	for (index = 0; index < (int)(sizeof(vector_types) / sizeof(vector_types[0])); index++)
	{
		if (strcmp(name, vector_types[index].glsl))
			continue;
		if (!comma_count)
		{
			/* one argument: a cast, which repeats a scalar as GLSL does */
			text_append(out, "((%s)(", vector_types[index].hlsl);
			convert_argument(converter, open, close, commas, comma_count, 0, out);
			text_append(out, "))");
		}
		else
		{
			text_append(out, "%s(", vector_types[index].hlsl);
			convert_range(converter, open + 1, close, out);
			text_append(out, ")");
		}
		return close + 1;
	}
	for (index = 0; index < (int)(sizeof(comparisons) / sizeof(comparisons[0])); index++)
	{
		if (strcmp(name, comparisons[index].glsl))
			continue;
		if (comma_count != 1)
			return converter_fail(converter, "%s with %d arguments", name, comma_count + 1), 0;
		text_append(out, "((");
		convert_argument(converter, open, close, commas, comma_count, 0, out);
		text_append(out, ") %s (", comparisons[index].operator);
		convert_argument(converter, open, close, commas, comma_count, 1, out);
		text_append(out, "))");
		return close + 1;
	}
	if (!strcmp(name, "texture"))
	{
		int stage = texture_argument(converter, open, close, commas, comma_count);

		if (stage < 0)
			return 0;
		if (comma_count == 1)
		{
			text_append(out, "tex%d_t.Sample(tex%d_s, ", stage, stage);
			convert_argument(converter, open, close, commas, comma_count, 1, out);
			text_append(out, ")");
		}
		else if (comma_count == 2)
		{
			text_append(out, "tex%d_t.SampleBias(tex%d_s, ", stage, stage);
			convert_argument(converter, open, close, commas, comma_count, 1, out);
			text_append(out, ", ");
			convert_argument(converter, open, close, commas, comma_count, 2, out);
			text_append(out, ")");
		}
		else
		{
			return converter_fail(converter, "texture with %d arguments", comma_count + 1), 0;
		}
		return close + 1;
	}
	if (!strcmp(name, "textureSize"))
	{
		int stage = texture_argument(converter, open, close, commas, comma_count);

		if (stage < 0)
			return 0;
		if (comma_count != 1 || converter->declarations->textures[stage] != _texture_2d)
			return converter_fail(converter, "textureSize of a texture that is not 2D"), 0;
		text_append(out, "xgpu_texture_size%d(", stage);
		convert_argument(converter, open, close, commas, comma_count, 1, out);
		text_append(out, ")");
		return close + 1;
	}
	if (!strcmp(name, "texelFetch"))
	{
		int stage = texture_argument(converter, open, close, commas, comma_count);

		if (stage < 0)
			return 0;
		if (comma_count != 2 || converter->declarations->textures[stage] != _texture_2d)
			return converter_fail(converter, "texelFetch of a texture that is not 2D"), 0;
		text_append(out, "tex%d_t.Load(int3(", stage);
		convert_argument(converter, open, close, commas, comma_count, 1, out);
		text_append(out, ", ");
		convert_argument(converter, open, close, commas, comma_count, 2, out);
		text_append(out, "))");
		return close + 1;
	}
	return 0;
}

static void convert_range(struct converter *converter, size_t begin, size_t end, struct hlsl_text *out)
{
	const char *source = converter->source;
	size_t position = begin;

	while (position < end && !converter->failed)
	{
		size_t skipped = comment_end(source, position, end);
		char c = source[position];

		if (skipped != position)
		{
			text_append_length(out, source + position, skipped - position);
			position = skipped;
			continue;
		}
		if (isdigit((unsigned char)c) || (c == '.' && position + 1 < end && isdigit((unsigned char)source[position + 1])))
		{
			/* a number, its exponent's sign included */
			size_t first = position;

			while (position < end && (isalnum((unsigned char)source[position]) || source[position] == '.' ||
				((source[position] == '+' || source[position] == '-') &&
				(source[position - 1] == 'e' || source[position - 1] == 'E'))))
			{
				position++;
			}
			text_append_length(out, source + first, position - first);
			continue;
		}
		if (is_identifier_start(c))
		{
			char name[64];
			size_t length = 0, after;
			const char *replacement;

			while (position < end && is_identifier_char(source[position]))
			{
				if (length < sizeof(name) - 1)
					name[length++] = source[position];
				position++;
			}
			name[length] = '\0';
			if (unknown_builtin(name))
			{
				converter_fail(converter, "%s, which the conversion does not know", name);
				return;
			}
			for (after = position; after < end && isspace((unsigned char)source[after]); after++)
				;
			if (after < end && source[after] == '(')
			{
				size_t next = convert_call(converter, name, after, end, out);

				if (converter->failed)
					return;
				if (next)
				{
					position = next;
					continue;
				}
			}
			replacement = renamed(name);
			if (replacement)
				text_append(out, "%s", replacement);
			else
				text_append(out, "%s_", name);
			continue;
		}
		text_append_length(out, &c, 1);
		position++;
	}
}

/* ---------- the whole shader */

static const char *texture_type(unsigned char type)
{
	switch (type)
	{
	case _texture_3d: return "Texture3D";
	case _texture_cube: return "TextureCube";
	default: return "Texture2D";
	}
}

static void declare_resources(struct hlsl_text *out, const struct declarations *declarations)
{
	int stage;

	text_append(out,
		"cbuffer xgpu_vertex_constants : register(b0)\n{\n\tfloat4 c[%d];\n};\n"
		"cbuffer xgpu_draw_uniforms : register(b1)\n{\n"
		"\tfloat4 viewport_scale;\n\tfloat4 viewport_offset;\n"
		"\tfloat4 ps_c0[8];\n\tfloat4 ps_c1[8];\n\tfloat4 ps_final_c0;\n\tfloat4 ps_final_c1;\n"
		"\tfloat4 fog_color;\n\tfloat4 fog_parameters;\n"
		"\tfloat4 bump_matrix[4];\n\tfloat4 bump_luminance[4];\n\tfloat4 texture_scale[4];\n"
		"\tfloat4 texture_lod_bias;\n\tfloat4 xgpu_misc;\n};\n"
		"cbuffer xgpu_model_lights : register(b2)\n{\n\tfloat4 model_lights[%d];\n};\n"
		"#define point_size (xgpu_misc.x)\n"
		"#define screen_offset (xgpu_misc.y)\n"
		"#define alpha_reference (xgpu_misc.z)\n",
		XGPU_HLSL_CONSTANT_COUNT, XGPU_HLSL_MODEL_LIGHT_COUNT);
	for (stage = 0; stage < 4; stage++)
	{
		if (!declarations->textures[stage])
			continue;
		text_append(out, "%s tex%d_t : register(t%d);\nSamplerState tex%d_s : register(s%d);\n",
			texture_type(declarations->textures[stage]), stage, stage, stage, stage);
		if (declarations->textures[stage] == _texture_2d)
		{
			text_append(out,
				"int2 xgpu_texture_size%d(int level)\n{\n"
				"\tuint width, height, levels;\n"
				"\ttex%d_t.GetDimensions((uint)level, width, height, levels);\n"
				"\treturn int2(width, height);\n}\n", stage, stage);
		}
	}
}

/* every varying, in one struct for both stages, so that a vertex shader's
outputs always match any pixel shader's inputs */
static void declare_varyings(struct hlsl_text *out)
{
	int index;

	text_append(out, "struct xgpu_varyings\n{\n\tfloat4 position : SV_Position;\n");
	for (index = 0; index < VARYING_COUNT; index++)
		text_append(out, "\t%s %s : %s;\n", varyings[index].type, varyings[index].name, varyings[index].semantic);
	text_append(out, "};\n");
	for (index = 0; index < VARYING_COUNT; index++)
		text_append(out, "static %s %s;\n", varyings[index].type, varyings[index].name);
}

static void vertex_entry(struct hlsl_text *out, const struct declarations *declarations,
	const unsigned char *input_kinds)
{
	int index;

	text_append(out, "struct xgpu_vertex_input\n{\n");
	for (index = 0; index < XGPU_HLSL_ATTRIBUTE_COUNT; index++)
	{
		unsigned char kind = input_kinds ? input_kinds[index] : _xgpu_hlsl_input_float;

		if (declarations->attributes[index] == 2)
			text_append(out, "\tuint v%d_packed : ATTRIB%d;\n", index, index);
		else if (declarations->attributes[index] == 1 && (kind == _xgpu_hlsl_input_sint || kind == _xgpu_hlsl_input_sint_w1))
			text_append(out, "\tint4 v%d_raw : ATTRIB%d;\n", index, index);
		else if (declarations->attributes[index] == 1)
			text_append(out, "\tfloat4 v%d_raw : ATTRIB%d;\n", index, index);
	}
	text_append(out, "};\n");
	text_append(out, "xgpu_varyings vs_main(xgpu_vertex_input input)\n{\n\txgpu_varyings output;\n");
	for (index = 0; index < XGPU_HLSL_ATTRIBUTE_COUNT; index++)
	{
		unsigned char kind = input_kinds ? input_kinds[index] : _xgpu_hlsl_input_float;

		if (declarations->attributes[index] == 2)
			text_append(out, "\tv%d_packed = input.v%d_packed;\n", index, index);
		else if (declarations->attributes[index] == 1 &&
			(kind == _xgpu_hlsl_input_float_w1 || kind == _xgpu_hlsl_input_sint_w1))
			text_append(out, "\tv%d_in = float4((float3)input.v%d_raw.xyz, 1.0);\n", index, index);
		else if (declarations->attributes[index] == 1)
			text_append(out, "\tv%d_in = (float4)input.v%d_raw;\n", index, index);
	}
	text_append(out, "\tglsl_main();\n\toutput.position = xgpu_position;\n");
	for (index = 0; index < VARYING_COUNT; index++)
	{
		if (declarations->varying_used[index])
			text_append(out, "\toutput.%s = %s;\n", varyings[index].name, varyings[index].name);
		else
			text_append(out, "\toutput.%s = (%s)0;\n", varyings[index].name, varyings[index].type);
	}
	text_append(out, "\treturn output;\n}\n");
}

static void pixel_entry(struct hlsl_text *out, const struct declarations *declarations, int sample_mask)
{
	int index;

	text_append(out, "struct xgpu_pixel_output\n{\n\tfloat4 color : SV_Target0;\n");
	if (sample_mask)
		text_append(out, "\tuint coverage : SV_Coverage;\n");
	text_append(out, "};\nxgpu_pixel_output ps_main(xgpu_varyings input)\n{\n\txgpu_pixel_output output;\n");
	for (index = 0; index < VARYING_COUNT; index++)
	{
		if (declarations->varying_used[index])
			text_append(out, "\t%s = input.%s;\n", varyings[index].name, varyings[index].name);
	}
	text_append(out, "\tglsl_main();\n\toutput.color = fragment_color;\n");
	if (sample_mask)
		text_append(out, "\toutput.coverage = (uint)xgpu_sample_mask[0];\n");
	text_append(out, "\treturn output;\n}\n");
}

char *xgpu_hlsl_from_glsl(const char *glsl, int pixel, const unsigned char *input_kinds, char *error,
	size_t error_size)
{
	struct declarations declarations;
	struct hlsl_text functions = { 0 }, out = { 0 };
	struct converter converter;
	const char *line = glsl;
	int depth = 0, index, sample_mask;
	size_t main_at;

	memset(&declarations, 0, sizeof(declarations));
	declarations.pixel = pixel;
	if (error && error_size)
		error[0] = '\0';

	/* the declarations at the top level are read; the rest (the
	functions) is kept, in GLSL still */
	while (*line)
	{
		const char *next = strchr(line, '\n');
		size_t length = next ? (size_t)(next - line) + 1 : strlen(line);
		const char *start = line;
		size_t position;

		while (*start == ' ' || *start == '\t')
			start++;
		if (!depth && *start != '\n' && *start && (starts_with(start, "#version") || starts_with(start, "precision") ||
			starts_with(start, "invariant") || starts_with(start, "uniform") || starts_with(start, "layout") ||
			starts_with(start, "in ") || starts_with(start, "out ")))
		{
			char declaration[256];
			size_t declaration_length = length < sizeof(declaration) ? length : sizeof(declaration) - 1;

			memcpy(declaration, start, declaration_length);
			declaration[declaration_length] = '\0';
			if (!read_declaration(declaration, &declarations, error, error_size))
			{
				free(functions.buffer);
				return NULL;
			}
		}
		else
		{
			text_append_length(&functions, line, length);
		}
		for (position = 0; position < length; position++)
		{
			if (line[position] == '{')
				depth++;
			else if (line[position] == '}')
				depth--;
		}
		line += length;
	}
	if (!functions.buffer || functions.failed)
	{
		free(functions.buffer);
		fail(error, error_size, "no functions, or out of memory");
		return NULL;
	}
	if (pixel && !declarations.fragment_color)
	{
		free(functions.buffer);
		fail(error, error_size, "a pixel shader without fragment_color");
		return NULL;
	}
	{
		const char *main_function = strstr(functions.buffer, "void main()");

		if (!main_function)
		{
			free(functions.buffer);
			fail(error, error_size, "no main function");
			return NULL;
		}
		main_at = (size_t)(main_function - functions.buffer);
	}
	sample_mask = strstr(functions.buffer, "gl_SampleMask") != NULL;

	declare_resources(&out, &declarations);
	declare_varyings(&out);
	if (pixel)
	{
		text_append(&out, "static float4 fragment_color;\nstatic int xgpu_sample_mask[1] = { -1 };\n");
	}
	else
	{
		text_append(&out, "static float4 xgpu_position;\nstatic float xgpu_point_size;\n");
		for (index = 0; index < XGPU_HLSL_ATTRIBUTE_COUNT; index++)
		{
			if (declarations.attributes[index] == 2)
				text_append(&out, "static uint v%d_packed;\n", index);
			else if (declarations.attributes[index] == 1)
				text_append(&out, "static float4 v%d_in;\n", index);
		}
	}

	memset(&converter, 0, sizeof(converter));
	converter.source = functions.buffer;
	converter.declarations = &declarations;
	converter.error = error;
	converter.error_size = error_size;
	/* (main is renamed: the HLSL's entry point calls it) */
	convert_range(&converter, 0, main_at, &out);
	text_append(&out, "void glsl_main()");
	convert_range(&converter, main_at + strlen("void main()"), functions.length, &out);
	free(functions.buffer);
	if (converter.failed)
	{
		free(out.buffer);
		return NULL;
	}

	if (pixel)
		pixel_entry(&out, &declarations, sample_mask);
	else
		vertex_entry(&out, &declarations, input_kinds);
	if (out.failed)
	{
		free(out.buffer);
		fail(error, error_size, "out of memory");
		return NULL;
	}
	return out.buffer;
}

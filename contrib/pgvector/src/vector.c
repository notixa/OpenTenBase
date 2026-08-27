#include "postgres.h"

#include <math.h>

#include "bitutils.h"
#include "bitvec.h"
#include "catalog/pg_type.h"
#include "common/shortest_dec.h"
#include "fmgr.h"
#include "halfutils.h"
#include "halfvec.h"
#include "hnsw.h"
#include "ivfflat.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "port.h"				/* for strtof() */
#include "sparsevec.h"
#include "utils/array.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/varbit.h"
#include "vector.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#if PG_VERSION_NUM >= 170000
#include "parser/scansup.h"
#endif

#if PG_VERSION_NUM >= 190000
#define palloc_array_checked(type, count) ((type *) palloc_array(type, count))
#else
#define palloc_array_checked(type, count) ((type *) palloc(mul_size(sizeof(type), count)))
#endif

#define STATE_DIMS(x) (ARR_DIMS(x)[0] - 1)
#define CreateStateDatums(dim) palloc_array_checked(Datum, (Size) ((dim) + 1))

#if defined(USE_TARGET_CLONES) && !defined(__FMA__)
#define VECTOR_TARGET_CLONES __attribute__((target_clones("default", "fma")))
#else
#define VECTOR_TARGET_CLONES
#endif

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(.name = "vector", .version = "0.8.6");
#else
PG_MODULE_MAGIC;
#endif

/*
 * Initialize index options and variables
 */
PGDLLEXPORT void _PG_init(void);
void
_PG_init(void)
{
	BitvecInit();
	HalfvecInit();
	HnswInit();
	IvfflatInit();
}

/*
 * Ensure same dimensions
 */
static inline void
CheckDims(Vector * a, Vector * b)
{
	if (a->dim != b->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different vector dimensions %d and %d", a->dim, b->dim)));
}

/*
 * Ensure expected dimensions
 */
static inline void
CheckExpectedDim(int32 typmod, int dim)
{
	if (typmod != -1 && typmod != dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, dim)));
}

/*
 * Ensure valid dimensions
 */
static inline void
CheckDim(int dim)
{
	if (dim < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	if (dim > VECTOR_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("vector cannot have more than %d dimensions", VECTOR_MAX_DIM)));
}

/*
 * Ensure finite element
 */
static inline void
CheckElement(float value)
{
	if (isnan(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("NaN not allowed in vector")));

	if (isinf(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("infinite value not allowed in vector")));
}

/*
 * Allocate and initialize a new vector
 */
Vector *
InitVector(int dim)
{
	Vector	   *result;
	Size		size;

	size = VECTOR_SIZE(dim);
	result = (Vector *) palloc0(size);
	SET_VARSIZE(result, size);
	result->dim = (int16) dim;

	return result;
}

#if PG_VERSION_NUM >= 170000
#define vector_isspace(ch) scanner_isspace(ch)
#else
static inline bool
vector_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\v' ||
		ch == '\f')
		return true;
	return false;
}
#endif

/*
 * Check state array
 */
static float8 *
CheckStateArray(ArrayType *statearray, const char *caller)
{
	if (ARR_NDIM(statearray) != 1 ||
		ARR_DIMS(statearray)[0] < 1 ||
		ARR_HASNULL(statearray) ||
		ARR_ELEMTYPE(statearray) != FLOAT8OID)
		elog(ERROR, "%s: expected state array", caller);
	return (float8 *) ARR_DATA_PTR(statearray);
}

/*
 * Convert textual representation to internal representation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_in);
Datum
vector_in(PG_FUNCTION_ARGS)
{
	char	   *lit = PG_GETARG_CSTRING(0);
	int32		typmod = PG_GETARG_INT32(2);
	float		x[VECTOR_MAX_DIM];
	int			dim = 0;
	char	   *pt = lit;
	Vector	   *result;

	while (vector_isspace(*pt))
		pt++;

	if (*pt != '[')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type vector: \"%s\"", lit),
				 errdetail("Vector contents must start with \"[\".")));

	pt++;

	while (vector_isspace(*pt))
		pt++;

	if (*pt == ']')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	for (;;)
	{
		float		val;
		char	   *stringEnd;

		if (dim == VECTOR_MAX_DIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("vector cannot have more than %d dimensions", VECTOR_MAX_DIM)));

		while (vector_isspace(*pt))
			pt++;

		/* Check for empty string like float4in */
		if (*pt == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));

		errno = 0;

		/* Use strtof like float4in to avoid a double-rounding problem */
		/* Postgres sets LC_NUMERIC to C on startup */
		val = strtof(pt, &stringEnd);

		if (stringEnd == pt)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));

		/* Check for range error like float4in */
		if (errno == ERANGE && isinf(val))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("\"%s\" is out of range for type vector", pnstrdup(pt, (Size) (stringEnd - pt)))));

		CheckElement(val);
		x[dim++] = val;

		pt = stringEnd;

		while (vector_isspace(*pt))
			pt++;

		if (*pt == ',')
			pt++;
		else if (*pt == ']')
		{
			pt++;
			break;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));
	}

	/* Only whitespace is allowed after the closing brace */
	while (vector_isspace(*pt))
		pt++;

	if (*pt != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type vector: \"%s\"", lit),
				 errdetail("Junk after closing right brace.")));

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
		result->x[i] = x[i];

	PG_RETURN_POINTER(result);
}

#define AppendChar(ptr, c) (*(ptr)++ = (c))
#define AppendFloat(ptr, f) ((ptr) += float_to_shortest_decimal_bufn((f), (ptr)))

/*
 * Convert internal representation to textual representation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_out);
Datum
vector_out(PG_FUNCTION_ARGS)
{
	Vector	   *vector = PG_GETARG_VECTOR_P(0);
	int			dim = vector->dim;
	char	   *buf;
	char	   *ptr;

	/*
	 * Need:
	 *
	 * dim * (FLOAT_SHORTEST_DECIMAL_LEN - 1) bytes for
	 * float_to_shortest_decimal_bufn
	 *
	 * max(dim - 1, 0) bytes for separator
	 *
	 * 3 bytes for [, ], and \0
	 */
	buf = (char *) palloc(add_size(mul_size(FLOAT_SHORTEST_DECIMAL_LEN, (Size) dim), 3));
	ptr = buf;

	AppendChar(ptr, '[');

	for (int i = 0; i < dim; i++)
	{
		if (i > 0)
			AppendChar(ptr, ',');

		AppendFloat(ptr, vector->x[i]);
	}

	AppendChar(ptr, ']');
	*ptr = '\0';

	PG_FREE_IF_COPY(vector, 0);
	PG_RETURN_CSTRING(buf);
}

/*
 * Print vector - useful for debugging
 */
void
PrintVector(char *msg, Vector * vector)
{
	char	   *out = DatumGetPointer(DirectFunctionCall1(vector_out, PointerGetDatum(vector)));

	elog(INFO, "%s = %s", msg, out);
	pfree(out);
}

/*
 * Convert type modifier
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_typmod_in);
Datum
vector_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type vector must be at least 1")));

	if (*tl > VECTOR_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type vector cannot exceed %d", VECTOR_MAX_DIM)));

	PG_RETURN_INT32(*tl);
}

/*
 * Convert external binary representation to internal representation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_recv);
Datum
vector_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int32		typmod = PG_GETARG_INT32(2);
	Vector	   *result;
	int			dim;
	int			unused;

	dim = (int) pq_getmsgint(buf, sizeof(int16));
	unused = (int) pq_getmsgint(buf, sizeof(int16));

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	if (unused != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected unused to be 0, not %d", unused)));

	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
	{
		result->x[i] = pq_getmsgfloat4(buf);
		CheckElement(result->x[i]);
	}

	PG_RETURN_POINTER(result);
}

/*
 * Convert internal representation to the external binary representation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_send);
Datum
vector_send(PG_FUNCTION_ARGS)
{
	Vector	   *vec = PG_GETARG_VECTOR_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint16(&buf, (uint16) vec->dim);
	pq_sendint16(&buf, (uint16) vec->unused);
	for (int i = 0; i < vec->dim; i++)
		pq_sendfloat4(&buf, vec->x[i]);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Convert vector to vector
 * This is needed to check the type modifier
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector);
Datum
vector(PG_FUNCTION_ARGS)
{
	Vector	   *vec = PG_GETARG_VECTOR_P(0);
	int32		typmod = PG_GETARG_INT32(1);

	CheckExpectedDim(typmod, vec->dim);

	PG_RETURN_POINTER(vec);
}

/*
 * Convert array to vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(array_to_vector);
Datum
array_to_vector(PG_FUNCTION_ARGS)
{
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Vector	   *result;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Datum	   *elemsp;
	int			nelemsp;

	if (ARR_NDIM(array) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("array must be 1-D")));

	if (ARR_HASNULL(array) && array_contains_nulls(array))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	get_typlenbyvalalign(ARR_ELEMTYPE(array), &typlen, &typbyval, &typalign);
	deconstruct_array(array, ARR_ELEMTYPE(array), typlen, typbyval, typalign, &elemsp, NULL, &nelemsp);

	CheckDim(nelemsp);
	CheckExpectedDim(typmod, nelemsp);

	result = InitVector(nelemsp);

	if (ARR_ELEMTYPE(array) == INT4OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = (float) DatumGetInt32(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == FLOAT8OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = (float) DatumGetFloat8(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == FLOAT4OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetFloat4(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == NUMERICOID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetFloat4(DirectFunctionCall1(numeric_float4, elemsp[i]));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("unsupported array type")));
	}

	/*
	 * Free allocation from deconstruct_array. Do not free individual elements
	 * when pass-by-reference since they point to original array.
	 */
	pfree(elemsp);

	/* Check elements */
	for (int i = 0; i < result->dim; i++)
		CheckElement(result->x[i]);

	PG_RETURN_POINTER(result);
}

/*
 * Convert vector to float4[]
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_to_float4);
Datum
vector_to_float4(PG_FUNCTION_ARGS)
{
	Vector	   *vec = PG_GETARG_VECTOR_P(0);
	Datum	   *datums;
	ArrayType  *result;

	datums = palloc_array_checked(Datum, (Size) vec->dim);

	for (int i = 0; i < vec->dim; i++)
		datums[i] = Float4GetDatum(vec->x[i]);

	/* Use TYPALIGN_INT for float4 */
	result = construct_array(datums, vec->dim, FLOAT4OID, sizeof(float4), true, TYPALIGN_INT);

	pfree(datums);

	PG_RETURN_POINTER(result);
}

/*
 * Convert half vector to vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(halfvec_to_vector);
Datum
halfvec_to_vector(PG_FUNCTION_ARGS)
{
	HalfVector *vec = PG_GETARG_HALFVEC_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Vector	   *result;

	CheckDim(vec->dim);
	CheckExpectedDim(typmod, vec->dim);

	result = InitVector(vec->dim);

	for (int i = 0; i < vec->dim; i++)
		result->x[i] = HalfToFloat4(vec->x[i]);

	PG_RETURN_POINTER(result);
}

#if defined(__x86_64__) || defined(__i386__)
static inline float __attribute__((target("avx2,fma")))
hsum256_ps(__m256 v)
{
	__m128 lo = _mm256_castps256_ps128(v);
	__m128 hi = _mm256_extractf128_ps(v, 1);
	__m128 s = _mm_add_ps(lo, hi);
	s = _mm_hadd_ps(s, s);
	s = _mm_hadd_ps(s, s);
	return _mm_cvtss_f32(s);
}

static inline float __attribute__((target("sse2")))
hsum128_ps(__m128 s)
{
	__m128 hi = _mm_movehl_ps(s, s);
	s = _mm_add_ps(s, hi);
	s = _mm_add_ss(s, _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1)));
	return _mm_cvtss_f32(s);
}

static inline float __attribute__((target("avx512f,avx512dq")))
hsum512_ps(__m512 acc)
{
	__m256 lo = _mm512_castps512_ps256(acc);
	__m256 hi = _mm512_extractf32x8_ps(acc, 1);
	__m256 s256 = _mm256_add_ps(lo, hi);
	__m128 s128 = _mm_add_ps(_mm256_castps256_ps128(s256), _mm256_extractf128_ps(s256, 1));
	s128 = _mm_hadd_ps(s128, s128);
	s128 = _mm_hadd_ps(s128, s128);
	return _mm_cvtss_f32(s128);
}
#endif

/*
 * L2 squared distance kernels. Runtime dispatch:
 *   x86: AVX512F -> AVX2+FMA -> SSE2 -> scalar (auto-vectorized)
 *   ARM: NEON (aarch64 / ARMv7+NEON)
 *   other: scalar (auto-vectorized)
 */
#if defined(__x86_64__) || defined(__i386__)
static float __attribute__((target("avx512f,avx512dq")))
VectorL2SquaredDistance_avx512f(int dim, float *ax, float *bx)
{
	float		distance = 0.0;
	int			i = 0;
	__m512		a0 = _mm512_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
	__m512		acc;
	__m256		lo, hi, s256;
	__m128		s128;

	for (; i + 63 < dim; i += 64)
	{
		__m512		d0 = _mm512_sub_ps(_mm512_loadu_ps(ax + i), _mm512_loadu_ps(bx + i));
		__m512		d1 = _mm512_sub_ps(_mm512_loadu_ps(ax + i + 16), _mm512_loadu_ps(bx + i + 16));
		__m512		d2 = _mm512_sub_ps(_mm512_loadu_ps(ax + i + 32), _mm512_loadu_ps(bx + i + 32));
		__m512		d3 = _mm512_sub_ps(_mm512_loadu_ps(ax + i + 48), _mm512_loadu_ps(bx + i + 48));

		a0 = _mm512_fmadd_ps(d0, d0, a0);
		a1 = _mm512_fmadd_ps(d1, d1, a1);
		a2 = _mm512_fmadd_ps(d2, d2, a2);
		a3 = _mm512_fmadd_ps(d3, d3, a3);
	}
	for (; i + 15 < dim; i += 16)
	{
		__m512		d = _mm512_sub_ps(_mm512_loadu_ps(ax + i), _mm512_loadu_ps(bx + i));

		a0 = _mm512_fmadd_ps(d, d, a0);
	}
	for (; i < dim; i++)
	{
		float		d = ax[i] - bx[i];

		distance += d * d;
	}

	acc = _mm512_add_ps(_mm512_add_ps(a0, a1), _mm512_add_ps(a2, a3));
	lo = _mm512_castps512_ps256(acc);
	hi = _mm512_extractf32x8_ps(acc, 1);
	s256 = _mm256_add_ps(lo, hi);
	s128 = _mm_add_ps(_mm256_castps256_ps128(s256), _mm256_extractf128_ps(s256, 1));
	s128 = _mm_hadd_ps(s128, s128);
	s128 = _mm_hadd_ps(s128, s128);
	distance += _mm_cvtss_f32(s128);
	return distance;
}

static float __attribute__((target("avx2,fma")))
VectorL2SquaredDistance_avx2(int dim, float *ax, float *bx)
{
	float		distance = 0.0;
	int			i = 0;
	__m256		a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
	__m256		acc;
	__m128		lo, hi, s;

	for (; i + 31 < dim; i += 32)
	{
		__m256		d0 = _mm256_sub_ps(_mm256_loadu_ps(ax + i), _mm256_loadu_ps(bx + i));
		__m256		d1 = _mm256_sub_ps(_mm256_loadu_ps(ax + i + 8), _mm256_loadu_ps(bx + i + 8));
		__m256		d2 = _mm256_sub_ps(_mm256_loadu_ps(ax + i + 16), _mm256_loadu_ps(bx + i + 16));
		__m256		d3 = _mm256_sub_ps(_mm256_loadu_ps(ax + i + 24), _mm256_loadu_ps(bx + i + 24));

		a0 = _mm256_fmadd_ps(d0, d0, a0);
		a1 = _mm256_fmadd_ps(d1, d1, a1);
		a2 = _mm256_fmadd_ps(d2, d2, a2);
		a3 = _mm256_fmadd_ps(d3, d3, a3);
	}
	for (; i + 7 < dim; i += 8)
	{
		__m256		d = _mm256_sub_ps(_mm256_loadu_ps(ax + i), _mm256_loadu_ps(bx + i));

		a0 = _mm256_fmadd_ps(d, d, a0);
	}
	for (; i < dim; i++)
	{
		float		d = ax[i] - bx[i];

		distance += d * d;
	}

	acc = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
	lo = _mm256_castps256_ps128(acc);
	hi = _mm256_extractf128_ps(acc, 1);
	s = _mm_add_ps(lo, hi);
	s = _mm_hadd_ps(s, s);
	s = _mm_hadd_ps(s, s);
	distance += _mm_cvtss_f32(s);
	return distance;
}

static float __attribute__((target("sse2")))
VectorL2SquaredDistance_sse2(int dim, float *ax, float *bx)
{
	float		distance = 0.0;
	int			i = 0;
	__m128		a0 = _mm_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
	__m128		s, hi;

	for (; i + 15 < dim; i += 16)
	{
		__m128		d0 = _mm_sub_ps(_mm_loadu_ps(ax + i), _mm_loadu_ps(bx + i));
		__m128		d1 = _mm_sub_ps(_mm_loadu_ps(ax + i + 4), _mm_loadu_ps(bx + i + 4));
		__m128		d2 = _mm_sub_ps(_mm_loadu_ps(ax + i + 8), _mm_loadu_ps(bx + i + 8));
		__m128		d3 = _mm_sub_ps(_mm_loadu_ps(ax + i + 12), _mm_loadu_ps(bx + i + 12));

		a0 = _mm_add_ps(_mm_mul_ps(d0, d0), a0);
		a1 = _mm_add_ps(_mm_mul_ps(d1, d1), a1);
		a2 = _mm_add_ps(_mm_mul_ps(d2, d2), a2);
		a3 = _mm_add_ps(_mm_mul_ps(d3, d3), a3);
	}
	for (; i + 3 < dim; i += 4)
	{
		__m128		d = _mm_sub_ps(_mm_loadu_ps(ax + i), _mm_loadu_ps(bx + i));

		a0 = _mm_add_ps(_mm_mul_ps(d, d), a0);
	}
	for (; i < dim; i++)
	{
		float		d = ax[i] - bx[i];

		distance += d * d;
	}

	s = _mm_add_ps(_mm_add_ps(a0, a1), _mm_add_ps(a2, a3));
	hi = _mm_movehl_ps(s, s);
	s = _mm_add_ps(s, hi);
	s = _mm_add_ss(s, _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1)));
	distance += _mm_cvtss_f32(s);
	return distance;
}
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
static float
VectorL2SquaredDistance_neon(int dim, float *ax, float *bx)
{
	float		distance = 0.0;
	int			i = 0;
	float32x4_t	a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
	float32x4_t	d0, d1, d2, d3, d;
	float32x4_t	sum;
	float32x2_t	lo, hi, sum2;

	for (; i + 15 < dim; i += 16)
	{
		d0 = vsubq_f32(vld1q_f32(ax + i), vld1q_f32(bx + i));
		d1 = vsubq_f32(vld1q_f32(ax + i + 4), vld1q_f32(bx + i + 4));
		d2 = vsubq_f32(vld1q_f32(ax + i + 8), vld1q_f32(bx + i + 8));
		d3 = vsubq_f32(vld1q_f32(ax + i + 12), vld1q_f32(bx + i + 12));

		a0 = vmlaq_f32(a0, d0, d0);
		a1 = vmlaq_f32(a1, d1, d1);
		a2 = vmlaq_f32(a2, d2, d2);
		a3 = vmlaq_f32(a3, d3, d3);
	}
	for (; i + 3 < dim; i += 4)
	{
		d = vsubq_f32(vld1q_f32(ax + i), vld1q_f32(bx + i));
		a0 = vmlaq_f32(a0, d, d);
	}
	for (; i < dim; i++)
	{
		float		d = ax[i] - bx[i];

		distance += d * d;
	}

	sum = vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3));
	lo = vget_low_f32(sum);
	hi = vget_high_f32(sum);
	sum2 = vadd_f32(lo, hi);
	sum2 = vpadd_f32(sum2, sum2);
	distance += vget_lane_f32(sum2, 0);
	return distance;
}
#endif

static float
VectorL2SquaredDistance_scalar(int dim, float *ax, float *bx)
{
	float		distance = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < dim; i++)
	{
		float		diff = ax[i] - bx[i];

		distance += diff * diff;
	}

	return distance;
}

static float
VectorL2SquaredDistance(int dim, float *ax, float *bx)
{
#if defined(__x86_64__) || defined(__i386__)
	static float	(*func) (int, float *, float *) = NULL;

	if (func == NULL)
	{
		if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq"))
			func = VectorL2SquaredDistance_avx512f;
		else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
			func = VectorL2SquaredDistance_avx2;
		else if (__builtin_cpu_supports("sse2"))
			func = VectorL2SquaredDistance_sse2;
		else
			func = VectorL2SquaredDistance_scalar;
	}
	return func(dim, ax, bx);
#elif defined(__aarch64__) || defined(__ARM_NEON)
	return VectorL2SquaredDistance_neon(dim, ax, bx);
#else
	return VectorL2SquaredDistance_scalar(dim, ax, bx);
#endif
}

/*
 * Get the L2 distance between vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l2_distance);
Datum
l2_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8(sqrt((double) VectorL2SquaredDistance(a->dim, a->x, b->x)));
}

/*
 * Get the L2 squared distance between vectors
 * This saves a sqrt calculation
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_l2_squared_distance);
Datum
vector_l2_squared_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double) VectorL2SquaredDistance(a->dim, a->x, b->x));
}


/*
 * 1-to-N Batch Distance Implementations
 */
#if defined(__x86_64__) || defined(__i386__)
static void __attribute__((target("avx512f,avx512dq")))
VectorBatchL2SquaredDistance_avx512f_4(int dim, const float *ax, const float * const *bx, double *distances)
{
	const float *bx0 = bx[0], *bx1 = bx[1], *bx2 = bx[2], *bx3 = bx[3];
	__m512 acc0_0 = _mm512_setzero_ps(), acc0_1 = _mm512_setzero_ps();
	__m512 acc1_0 = _mm512_setzero_ps(), acc1_1 = _mm512_setzero_ps();
	__m512 acc2_0 = _mm512_setzero_ps(), acc2_1 = _mm512_setzero_ps();
	__m512 acc3_0 = _mm512_setzero_ps(), acc3_1 = _mm512_setzero_ps();
	int i = 0;

	for (; i + 31 < dim; i += 32)
	{
		__m512 q0 = _mm512_loadu_ps(ax + i);
		__m512 q1 = _mm512_loadu_ps(ax + i + 16);
		__m512 d0_0 = _mm512_sub_ps(q0, _mm512_loadu_ps(bx0 + i));
		__m512 d0_1 = _mm512_sub_ps(q1, _mm512_loadu_ps(bx0 + i + 16));
		__m512 d1_0 = _mm512_sub_ps(q0, _mm512_loadu_ps(bx1 + i));
		__m512 d1_1 = _mm512_sub_ps(q1, _mm512_loadu_ps(bx1 + i + 16));
		__m512 d2_0 = _mm512_sub_ps(q0, _mm512_loadu_ps(bx2 + i));
		__m512 d2_1 = _mm512_sub_ps(q1, _mm512_loadu_ps(bx2 + i + 16));
		__m512 d3_0 = _mm512_sub_ps(q0, _mm512_loadu_ps(bx3 + i));
		__m512 d3_1 = _mm512_sub_ps(q1, _mm512_loadu_ps(bx3 + i + 16));

		acc0_0 = _mm512_fmadd_ps(d0_0, d0_0, acc0_0);
		acc0_1 = _mm512_fmadd_ps(d0_1, d0_1, acc0_1);
		acc1_0 = _mm512_fmadd_ps(d1_0, d1_0, acc1_0);
		acc1_1 = _mm512_fmadd_ps(d1_1, d1_1, acc1_1);
		acc2_0 = _mm512_fmadd_ps(d2_0, d2_0, acc2_0);
		acc2_1 = _mm512_fmadd_ps(d2_1, d2_1, acc2_1);
		acc3_0 = _mm512_fmadd_ps(d3_0, d3_0, acc3_0);
		acc3_1 = _mm512_fmadd_ps(d3_1, d3_1, acc3_1);
	}
	for (; i + 15 < dim; i += 16)
	{
		__m512 q0 = _mm512_loadu_ps(ax + i);
		__m512 d0 = _mm512_sub_ps(q0, _mm512_loadu_ps(bx0 + i));
		__m512 d1 = _mm512_sub_ps(q0, _mm512_loadu_ps(bx1 + i));
		__m512 d2 = _mm512_sub_ps(q0, _mm512_loadu_ps(bx2 + i));
		__m512 d3 = _mm512_sub_ps(q0, _mm512_loadu_ps(bx3 + i));

		acc0_0 = _mm512_fmadd_ps(d0, d0, acc0_0);
		acc1_0 = _mm512_fmadd_ps(d1, d1, acc1_0);
		acc2_0 = _mm512_fmadd_ps(d2, d2, acc2_0);
		acc3_0 = _mm512_fmadd_ps(d3, d3, acc3_0);
	}

	acc0_0 = _mm512_add_ps(acc0_0, acc0_1);
	acc1_0 = _mm512_add_ps(acc1_0, acc1_1);
	acc2_0 = _mm512_add_ps(acc2_0, acc2_1);
	acc3_0 = _mm512_add_ps(acc3_0, acc3_1);

	distances[0] = (double) hsum512_ps(acc0_0);
	distances[1] = (double) hsum512_ps(acc1_0);
	distances[2] = (double) hsum512_ps(acc2_0);
	distances[3] = (double) hsum512_ps(acc3_0);

	for (; i < dim; i++)
	{
		float q = ax[i];
		float d0 = q - bx0[i], d1 = q - bx1[i], d2 = q - bx2[i], d3 = q - bx3[i];
		distances[0] += d0 * d0;
		distances[1] += d1 * d1;
		distances[2] += d2 * d2;
		distances[3] += d3 * d3;
	}
}

static void __attribute__((target("avx512f,avx512dq")))
VectorBatchL2SquaredDistance_avx512f(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	int k = 0;
	for (; k + 3 < count; k += 4)
		VectorBatchL2SquaredDistance_avx512f_4(dim, ax, bx + k, distances + k);
	for (; k < count; k++)
		distances[k] = (double) VectorL2SquaredDistance_avx512f(dim, (float *) ax, (float *) bx[k]);
}

static void __attribute__((target("avx2,fma")))
VectorBatchL2SquaredDistance_avx2_4(int dim, const float *ax, const float * const *bx, double *distances)
{
	const float *bx0 = bx[0], *bx1 = bx[1], *bx2 = bx[2], *bx3 = bx[3];
	__m256 acc0_0 = _mm256_setzero_ps(), acc0_1 = _mm256_setzero_ps();
	__m256 acc1_0 = _mm256_setzero_ps(), acc1_1 = _mm256_setzero_ps();
	__m256 acc2_0 = _mm256_setzero_ps(), acc2_1 = _mm256_setzero_ps();
	__m256 acc3_0 = _mm256_setzero_ps(), acc3_1 = _mm256_setzero_ps();
	int i = 0;

	for (; i + 15 < dim; i += 16)
	{
		__m256 q0 = _mm256_loadu_ps(ax + i);
		__m256 q1 = _mm256_loadu_ps(ax + i + 8);
		__m256 d0_0 = _mm256_sub_ps(q0, _mm256_loadu_ps(bx0 + i));
		__m256 d0_1 = _mm256_sub_ps(q1, _mm256_loadu_ps(bx0 + i + 8));
		__m256 d1_0 = _mm256_sub_ps(q0, _mm256_loadu_ps(bx1 + i));
		__m256 d1_1 = _mm256_sub_ps(q1, _mm256_loadu_ps(bx1 + i + 8));
		__m256 d2_0 = _mm256_sub_ps(q0, _mm256_loadu_ps(bx2 + i));
		__m256 d2_1 = _mm256_sub_ps(q1, _mm256_loadu_ps(bx2 + i + 8));
		__m256 d3_0 = _mm256_sub_ps(q0, _mm256_loadu_ps(bx3 + i));
		__m256 d3_1 = _mm256_sub_ps(q1, _mm256_loadu_ps(bx3 + i + 8));

		acc0_0 = _mm256_fmadd_ps(d0_0, d0_0, acc0_0);
		acc0_1 = _mm256_fmadd_ps(d0_1, d0_1, acc0_1);
		acc1_0 = _mm256_fmadd_ps(d1_0, d1_0, acc1_0);
		acc1_1 = _mm256_fmadd_ps(d1_1, d1_1, acc1_1);
		acc2_0 = _mm256_fmadd_ps(d2_0, d2_0, acc2_0);
		acc2_1 = _mm256_fmadd_ps(d2_1, d2_1, acc2_1);
		acc3_0 = _mm256_fmadd_ps(d3_0, d3_0, acc3_0);
		acc3_1 = _mm256_fmadd_ps(d3_1, d3_1, acc3_1);
	}
	for (; i + 7 < dim; i += 8)
	{
		__m256 q0 = _mm256_loadu_ps(ax + i);
		__m256 d0 = _mm256_sub_ps(q0, _mm256_loadu_ps(bx0 + i));
		__m256 d1 = _mm256_sub_ps(q0, _mm256_loadu_ps(bx1 + i));
		__m256 d2 = _mm256_sub_ps(q0, _mm256_loadu_ps(bx2 + i));
		__m256 d3 = _mm256_sub_ps(q0, _mm256_loadu_ps(bx3 + i));

		acc0_0 = _mm256_fmadd_ps(d0, d0, acc0_0);
		acc1_0 = _mm256_fmadd_ps(d1, d1, acc1_0);
		acc2_0 = _mm256_fmadd_ps(d2, d2, acc2_0);
		acc3_0 = _mm256_fmadd_ps(d3, d3, acc3_0);
	}

	acc0_0 = _mm256_add_ps(acc0_0, acc0_1);
	acc1_0 = _mm256_add_ps(acc1_0, acc1_1);
	acc2_0 = _mm256_add_ps(acc2_0, acc2_1);
	acc3_0 = _mm256_add_ps(acc3_0, acc3_1);

	distances[0] = (double) hsum256_ps(acc0_0);
	distances[1] = (double) hsum256_ps(acc1_0);
	distances[2] = (double) hsum256_ps(acc2_0);
	distances[3] = (double) hsum256_ps(acc3_0);

	for (; i < dim; i++)
	{
		float q = ax[i];
		float d0 = q - bx0[i], d1 = q - bx1[i], d2 = q - bx2[i], d3 = q - bx3[i];
		distances[0] += d0 * d0;
		distances[1] += d1 * d1;
		distances[2] += d2 * d2;
		distances[3] += d3 * d3;
	}
}

static void __attribute__((target("avx2,fma")))
VectorBatchL2SquaredDistance_avx2(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	int k = 0;
	for (; k + 3 < count; k += 4)
		VectorBatchL2SquaredDistance_avx2_4(dim, ax, bx + k, distances + k);
	for (; k < count; k++)
		distances[k] = (double) VectorL2SquaredDistance_avx2(dim, (float *) ax, (float *) bx[k]);
}

static void __attribute__((target("sse2")))
VectorBatchL2SquaredDistance_sse2_4(int dim, const float *ax, const float * const *bx, double *distances)
{
	const float *bx0 = bx[0], *bx1 = bx[1], *bx2 = bx[2], *bx3 = bx[3];
	__m128 acc0 = _mm_setzero_ps();
	__m128 acc1 = _mm_setzero_ps();
	__m128 acc2 = _mm_setzero_ps();
	__m128 acc3 = _mm_setzero_ps();
	int i = 0;

	for (; i + 3 < dim; i += 4)
	{
		__m128 q = _mm_loadu_ps(ax + i);
		__m128 d0 = _mm_sub_ps(q, _mm_loadu_ps(bx0 + i));
		__m128 d1 = _mm_sub_ps(q, _mm_loadu_ps(bx1 + i));
		__m128 d2 = _mm_sub_ps(q, _mm_loadu_ps(bx2 + i));
		__m128 d3 = _mm_sub_ps(q, _mm_loadu_ps(bx3 + i));

		acc0 = _mm_add_ps(acc0, _mm_mul_ps(d0, d0));
		acc1 = _mm_add_ps(acc1, _mm_mul_ps(d1, d1));
		acc2 = _mm_add_ps(acc2, _mm_mul_ps(d2, d2));
		acc3 = _mm_add_ps(acc3, _mm_mul_ps(d3, d3));
	}

	distances[0] = (double) hsum128_ps(acc0);
	distances[1] = (double) hsum128_ps(acc1);
	distances[2] = (double) hsum128_ps(acc2);
	distances[3] = (double) hsum128_ps(acc3);

	for (; i < dim; i++)
	{
		float q = ax[i];
		float d0 = q - bx0[i], d1 = q - bx1[i], d2 = q - bx2[i], d3 = q - bx3[i];
		distances[0] += d0 * d0;
		distances[1] += d1 * d1;
		distances[2] += d2 * d2;
		distances[3] += d3 * d3;
	}
}

static void __attribute__((target("sse2")))
VectorBatchL2SquaredDistance_sse2(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	int k = 0;
	for (; k + 3 < count; k += 4)
		VectorBatchL2SquaredDistance_sse2_4(dim, ax, bx + k, distances + k);
	for (; k < count; k++)
		distances[k] = (double) VectorL2SquaredDistance_sse2(dim, (float *) ax, (float *) bx[k]);
}
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
static void
VectorBatchL2SquaredDistance_neon_4(int dim, const float *ax, const float * const *bx, double *distances)
{
	const float *bx0 = bx[0], *bx1 = bx[1], *bx2 = bx[2], *bx3 = bx[3];
	float32x4_t acc0 = vdupq_n_f32(0.0f);
	float32x4_t acc1 = vdupq_n_f32(0.0f);
	float32x4_t acc2 = vdupq_n_f32(0.0f);
	float32x4_t acc3 = vdupq_n_f32(0.0f);
	int i = 0;

	for (; i + 3 < dim; i += 4)
	{
		float32x4_t q = vld1q_f32(ax + i);

		float32x4_t d0 = vsubq_f32(q, vld1q_f32(bx0 + i));
		acc0 = vmlaq_f32(acc0, d0, d0);

		float32x4_t d1 = vsubq_f32(q, vld1q_f32(bx1 + i));
		acc1 = vmlaq_f32(acc1, d1, d1);

		float32x4_t d2 = vsubq_f32(q, vld1q_f32(bx2 + i));
		acc2 = vmlaq_f32(acc2, d2, d2);

		float32x4_t d3 = vsubq_f32(q, vld1q_f32(bx3 + i));
		acc3 = vmlaq_f32(acc3, d3, d3);
	}

	distances[0] = (double) (vgetq_lane_f32(acc0, 0) + vgetq_lane_f32(acc0, 1) + vgetq_lane_f32(acc0, 2) + vgetq_lane_f32(acc0, 3));
	distances[1] = (double) (vgetq_lane_f32(acc1, 0) + vgetq_lane_f32(acc1, 1) + vgetq_lane_f32(acc1, 2) + vgetq_lane_f32(acc1, 3));
	distances[2] = (double) (vgetq_lane_f32(acc2, 0) + vgetq_lane_f32(acc2, 1) + vgetq_lane_f32(acc2, 2) + vgetq_lane_f32(acc2, 3));
	distances[3] = (double) (vgetq_lane_f32(acc3, 0) + vgetq_lane_f32(acc3, 1) + vgetq_lane_f32(acc3, 2) + vgetq_lane_f32(acc3, 3));

	for (; i < dim; i++)
	{
		float q = ax[i];
		float d0 = q - bx0[i], d1 = q - bx1[i], d2 = q - bx2[i], d3 = q - bx3[i];
		distances[0] += d0 * d0;
		distances[1] += d1 * d1;
		distances[2] += d2 * d2;
		distances[3] += d3 * d3;
	}
}

static void
VectorBatchL2SquaredDistance_neon(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	int k = 0;
	for (; k + 3 < count; k += 4)
		VectorBatchL2SquaredDistance_neon_4(dim, ax, bx + k, distances + k);
	for (; k < count; k++)
		distances[k] = (double) VectorL2SquaredDistance_neon(dim, (float *) ax, (float *) bx[k]);
}
#endif

static void
VectorBatchL2SquaredDistance_scalar(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	for (int k = 0; k < count; k++)
	{
		float dist = 0.0;
		const float *b = bx[k];
		for (int i = 0; i < dim; i++)
		{
			float diff = ax[i] - b[i];
			dist += diff * diff;
		}
		distances[k] = (double) dist;
	}
}

void
VectorBatchL2SquaredDistance(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
#if defined(__x86_64__) || defined(__i386__)
	static void (*func) (int, const float *, const float * const *, double *, int) = NULL;

	if (func == NULL)
	{
		if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq"))
			func = VectorBatchL2SquaredDistance_avx512f;
		else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
			func = VectorBatchL2SquaredDistance_avx2;
		else if (__builtin_cpu_supports("sse2"))
			func = VectorBatchL2SquaredDistance_sse2;
		else
			func = VectorBatchL2SquaredDistance_scalar;
	}
	func(dim, ax, bx, distances, count);
#elif defined(__aarch64__) || defined(__ARM_NEON)
	VectorBatchL2SquaredDistance_neon(dim, ax, bx, distances, count);
#else
	VectorBatchL2SquaredDistance_scalar(dim, ax, bx, distances, count);
#endif
}

void
VectorBatchL2Distance(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	VectorBatchL2SquaredDistance(dim, ax, bx, distances, count);
	for (int i = 0; i < count; i++)
		distances[i] = sqrt(distances[i]);
}

/*
 * Inner product kernels. Runtime dispatch:
 *   x86: AVX512F -> AVX2+FMA -> SSE2 -> scalar (auto-vectorized)
 *   ARM: NEON (aarch64 / ARMv7+NEON)
 *   other: scalar (auto-vectorized)
 */
#if defined(__x86_64__) || defined(__i386__)
static float __attribute__((target("avx512f,avx512dq")))
VectorInnerProduct_avx512f(int dim, float *ax, float *bx)
{
	float		distance = 0.0;
	int			i = 0;
	__m512		a0 = _mm512_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
	__m512		acc;
	__m256		lo, hi, s256;
	__m128		s128;

	for (; i + 63 < dim; i += 64)
	{
		a0 = _mm512_fmadd_ps(_mm512_loadu_ps(ax + i), _mm512_loadu_ps(bx + i), a0);
		a1 = _mm512_fmadd_ps(_mm512_loadu_ps(ax + i + 16), _mm512_loadu_ps(bx + i + 16), a1);
		a2 = _mm512_fmadd_ps(_mm512_loadu_ps(ax + i + 32), _mm512_loadu_ps(bx + i + 32), a2);
		a3 = _mm512_fmadd_ps(_mm512_loadu_ps(ax + i + 48), _mm512_loadu_ps(bx + i + 48), a3);
	}
	for (; i + 15 < dim; i += 16)
		a0 = _mm512_fmadd_ps(_mm512_loadu_ps(ax + i), _mm512_loadu_ps(bx + i), a0);
	for (; i < dim; i++)
		distance += ax[i] * bx[i];

	acc = _mm512_add_ps(_mm512_add_ps(a0, a1), _mm512_add_ps(a2, a3));
	lo = _mm512_castps512_ps256(acc);
	hi = _mm512_extractf32x8_ps(acc, 1);
	s256 = _mm256_add_ps(lo, hi);
	s128 = _mm_add_ps(_mm256_castps256_ps128(s256), _mm256_extractf128_ps(s256, 1));
	s128 = _mm_hadd_ps(s128, s128);
	s128 = _mm_hadd_ps(s128, s128);
	distance += _mm_cvtss_f32(s128);
	return distance;
}

static float __attribute__((target("avx2,fma")))
VectorInnerProduct_avx2(int dim, float *ax, float *bx)
{
	float		distance = 0.0;
	int			i = 0;
	__m256		a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
	__m256		acc;
	__m128		lo, hi, s;

	for (; i + 31 < dim; i += 32)
	{
		a0 = _mm256_fmadd_ps(_mm256_loadu_ps(ax + i), _mm256_loadu_ps(bx + i), a0);
		a1 = _mm256_fmadd_ps(_mm256_loadu_ps(ax + i + 8), _mm256_loadu_ps(bx + i + 8), a1);
		a2 = _mm256_fmadd_ps(_mm256_loadu_ps(ax + i + 16), _mm256_loadu_ps(bx + i + 16), a2);
		a3 = _mm256_fmadd_ps(_mm256_loadu_ps(ax + i + 24), _mm256_loadu_ps(bx + i + 24), a3);
	}
	for (; i + 7 < dim; i += 8)
		a0 = _mm256_fmadd_ps(_mm256_loadu_ps(ax + i), _mm256_loadu_ps(bx + i), a0);
	for (; i < dim; i++)
		distance += ax[i] * bx[i];

	acc = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
	lo = _mm256_castps256_ps128(acc);
	hi = _mm256_extractf128_ps(acc, 1);
	s = _mm_add_ps(lo, hi);
	s = _mm_hadd_ps(s, s);
	s = _mm_hadd_ps(s, s);
	distance += _mm_cvtss_f32(s);
	return distance;
}

static float __attribute__((target("sse2")))
VectorInnerProduct_sse2(int dim, float *ax, float *bx)
{
	float		distance = 0.0;
	int			i = 0;
	__m128		a0 = _mm_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
	__m128		s, hi;

	for (; i + 15 < dim; i += 16)
	{
		a0 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(ax + i), _mm_loadu_ps(bx + i)), a0);
		a1 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(ax + i + 4), _mm_loadu_ps(bx + i + 4)), a1);
		a2 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(ax + i + 8), _mm_loadu_ps(bx + i + 8)), a2);
		a3 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(ax + i + 12), _mm_loadu_ps(bx + i + 12)), a3);
	}
	for (; i + 3 < dim; i += 4)
		a0 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(ax + i), _mm_loadu_ps(bx + i)), a0);
	for (; i < dim; i++)
		distance += ax[i] * bx[i];

	s = _mm_add_ps(_mm_add_ps(a0, a1), _mm_add_ps(a2, a3));
	hi = _mm_movehl_ps(s, s);
	s = _mm_add_ps(s, hi);
	s = _mm_add_ss(s, _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1)));
	distance += _mm_cvtss_f32(s);
	return distance;
}
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
static float
VectorInnerProduct_neon(int dim, float *ax, float *bx)
{
	float		distance = 0.0;
	int			i = 0;
	float32x4_t	a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
	float32x4_t	sum;
	float32x2_t	lo, hi, sum2;

	for (; i + 15 < dim; i += 16)
	{
		a0 = vmlaq_f32(a0, vld1q_f32(ax + i), vld1q_f32(bx + i));
		a1 = vmlaq_f32(a1, vld1q_f32(ax + i + 4), vld1q_f32(bx + i + 4));
		a2 = vmlaq_f32(a2, vld1q_f32(ax + i + 8), vld1q_f32(bx + i + 8));
		a3 = vmlaq_f32(a3, vld1q_f32(ax + i + 12), vld1q_f32(bx + i + 12));
	}
	for (; i + 3 < dim; i += 4)
		a0 = vmlaq_f32(a0, vld1q_f32(ax + i), vld1q_f32(bx + i));
	for (; i < dim; i++)
		distance += ax[i] * bx[i];

	sum = vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3));
	lo = vget_low_f32(sum);
	hi = vget_high_f32(sum);
	sum2 = vadd_f32(lo, hi);
	sum2 = vpadd_f32(sum2, sum2);
	distance += vget_lane_f32(sum2, 0);
	return distance;
}
#endif

static float
VectorInnerProduct_scalar(int dim, float *ax, float *bx)
{
	float		distance = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < dim; i++)
		distance += ax[i] * bx[i];

	return distance;
}

static float
VectorInnerProduct(int dim, float *ax, float *bx)
{
#if defined(__x86_64__) || defined(__i386__)
	static float	(*func) (int, float *, float *) = NULL;

	if (func == NULL)
	{
		if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq"))
			func = VectorInnerProduct_avx512f;
		else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
			func = VectorInnerProduct_avx2;
		else if (__builtin_cpu_supports("sse2"))
			func = VectorInnerProduct_sse2;
		else
			func = VectorInnerProduct_scalar;
	}
	return func(dim, ax, bx);
#elif defined(__aarch64__) || defined(__ARM_NEON)
	return VectorInnerProduct_neon(dim, ax, bx);
#else
	return VectorInnerProduct_scalar(dim, ax, bx);
#endif
}

/*
 * Get the inner product of two vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(inner_product);
Datum
inner_product(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double) VectorInnerProduct(a->dim, a->x, b->x));
}

/*
 * Get the negative inner product of two vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_negative_inner_product);
Datum
vector_negative_inner_product(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double) -VectorInnerProduct(a->dim, a->x, b->x));
}


#if defined(__x86_64__) || defined(__i386__)
static void __attribute__((target("avx512f,avx512dq")))
VectorBatchNegativeInnerProduct_avx512f_4(int dim, const float *ax, const float * const *bx, double *distances)
{
	const float *bx0 = bx[0], *bx1 = bx[1], *bx2 = bx[2], *bx3 = bx[3];
	__m512 acc0_0 = _mm512_setzero_ps(), acc0_1 = _mm512_setzero_ps();
	__m512 acc1_0 = _mm512_setzero_ps(), acc1_1 = _mm512_setzero_ps();
	__m512 acc2_0 = _mm512_setzero_ps(), acc2_1 = _mm512_setzero_ps();
	__m512 acc3_0 = _mm512_setzero_ps(), acc3_1 = _mm512_setzero_ps();
	int i = 0;

	for (; i + 31 < dim; i += 32)
	{
		__m512 q0 = _mm512_loadu_ps(ax + i);
		__m512 q1 = _mm512_loadu_ps(ax + i + 16);

		acc0_0 = _mm512_fmadd_ps(q0, _mm512_loadu_ps(bx0 + i), acc0_0);
		acc0_1 = _mm512_fmadd_ps(q1, _mm512_loadu_ps(bx0 + i + 16), acc0_1);
		acc1_0 = _mm512_fmadd_ps(q0, _mm512_loadu_ps(bx1 + i), acc1_0);
		acc1_1 = _mm512_fmadd_ps(q1, _mm512_loadu_ps(bx1 + i + 16), acc1_1);
		acc2_0 = _mm512_fmadd_ps(q0, _mm512_loadu_ps(bx2 + i), acc2_0);
		acc2_1 = _mm512_fmadd_ps(q1, _mm512_loadu_ps(bx2 + i + 16), acc2_1);
		acc3_0 = _mm512_fmadd_ps(q0, _mm512_loadu_ps(bx3 + i), acc3_0);
		acc3_1 = _mm512_fmadd_ps(q1, _mm512_loadu_ps(bx3 + i + 16), acc3_1);
	}
	for (; i + 15 < dim; i += 16)
	{
		__m512 q0 = _mm512_loadu_ps(ax + i);

		acc0_0 = _mm512_fmadd_ps(q0, _mm512_loadu_ps(bx0 + i), acc0_0);
		acc1_0 = _mm512_fmadd_ps(q0, _mm512_loadu_ps(bx1 + i), acc1_0);
		acc2_0 = _mm512_fmadd_ps(q0, _mm512_loadu_ps(bx2 + i), acc2_0);
		acc3_0 = _mm512_fmadd_ps(q0, _mm512_loadu_ps(bx3 + i), acc3_0);
	}

	acc0_0 = _mm512_add_ps(acc0_0, acc0_1);
	acc1_0 = _mm512_add_ps(acc1_0, acc1_1);
	acc2_0 = _mm512_add_ps(acc2_0, acc2_1);
	acc3_0 = _mm512_add_ps(acc3_0, acc3_1);

	distances[0] = -(double) hsum512_ps(acc0_0);
	distances[1] = -(double) hsum512_ps(acc1_0);
	distances[2] = -(double) hsum512_ps(acc2_0);
	distances[3] = -(double) hsum512_ps(acc3_0);

	for (; i < dim; i++)
	{
		float q = ax[i];
		distances[0] -= (double)(q * bx0[i]);
		distances[1] -= (double)(q * bx1[i]);
		distances[2] -= (double)(q * bx2[i]);
		distances[3] -= (double)(q * bx3[i]);
	}
}

static void __attribute__((target("avx512f,avx512dq")))
VectorBatchNegativeInnerProduct_avx512f(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	int k = 0;
	for (; k + 3 < count; k += 4)
		VectorBatchNegativeInnerProduct_avx512f_4(dim, ax, bx + k, distances + k);
	for (; k < count; k++)
		distances[k] = -(double) VectorInnerProduct_avx512f(dim, (float *) ax, (float *) bx[k]);
}

static void __attribute__((target("avx2,fma")))
VectorBatchNegativeInnerProduct_avx2_4(int dim, const float *ax, const float * const *bx, double *distances)
{
	const float *bx0 = bx[0], *bx1 = bx[1], *bx2 = bx[2], *bx3 = bx[3];
	__m256 acc0_0 = _mm256_setzero_ps(), acc0_1 = _mm256_setzero_ps();
	__m256 acc1_0 = _mm256_setzero_ps(), acc1_1 = _mm256_setzero_ps();
	__m256 acc2_0 = _mm256_setzero_ps(), acc2_1 = _mm256_setzero_ps();
	__m256 acc3_0 = _mm256_setzero_ps(), acc3_1 = _mm256_setzero_ps();
	int i = 0;

	for (; i + 15 < dim; i += 16)
	{
		__m256 q0 = _mm256_loadu_ps(ax + i);
		__m256 q1 = _mm256_loadu_ps(ax + i + 8);

		acc0_0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(bx0 + i), acc0_0);
		acc0_1 = _mm256_fmadd_ps(q1, _mm256_loadu_ps(bx0 + i + 8), acc0_1);
		acc1_0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(bx1 + i), acc1_0);
		acc1_1 = _mm256_fmadd_ps(q1, _mm256_loadu_ps(bx1 + i + 8), acc1_1);
		acc2_0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(bx2 + i), acc2_0);
		acc2_1 = _mm256_fmadd_ps(q1, _mm256_loadu_ps(bx2 + i + 8), acc2_1);
		acc3_0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(bx3 + i), acc3_0);
		acc3_1 = _mm256_fmadd_ps(q1, _mm256_loadu_ps(bx3 + i + 8), acc3_1);
	}
	for (; i + 7 < dim; i += 8)
	{
		__m256 q0 = _mm256_loadu_ps(ax + i);

		acc0_0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(bx0 + i), acc0_0);
		acc1_0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(bx1 + i), acc1_0);
		acc2_0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(bx2 + i), acc2_0);
		acc3_0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(bx3 + i), acc3_0);
	}

	acc0_0 = _mm256_add_ps(acc0_0, acc0_1);
	acc1_0 = _mm256_add_ps(acc1_0, acc1_1);
	acc2_0 = _mm256_add_ps(acc2_0, acc2_1);
	acc3_0 = _mm256_add_ps(acc3_0, acc3_1);

	distances[0] = -(double) hsum256_ps(acc0_0);
	distances[1] = -(double) hsum256_ps(acc1_0);
	distances[2] = -(double) hsum256_ps(acc2_0);
	distances[3] = -(double) hsum256_ps(acc3_0);

	for (; i < dim; i++)
	{
		float q = ax[i];
		distances[0] -= (double)(q * bx0[i]);
		distances[1] -= (double)(q * bx1[i]);
		distances[2] -= (double)(q * bx2[i]);
		distances[3] -= (double)(q * bx3[i]);
	}
}

static void __attribute__((target("avx2,fma")))
VectorBatchNegativeInnerProduct_avx2(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	int k = 0;
	for (; k + 3 < count; k += 4)
		VectorBatchNegativeInnerProduct_avx2_4(dim, ax, bx + k, distances + k);
	for (; k < count; k++)
		distances[k] = -(double) VectorInnerProduct_avx2(dim, (float *) ax, (float *) bx[k]);
}

static void __attribute__((target("sse2")))
VectorBatchNegativeInnerProduct_sse2_4(int dim, const float *ax, const float * const *bx, double *distances)
{
	const float *bx0 = bx[0], *bx1 = bx[1], *bx2 = bx[2], *bx3 = bx[3];
	__m128 acc0 = _mm_setzero_ps();
	__m128 acc1 = _mm_setzero_ps();
	__m128 acc2 = _mm_setzero_ps();
	__m128 acc3 = _mm_setzero_ps();
	int i = 0;

	for (; i + 3 < dim; i += 4)
	{
		__m128 q = _mm_loadu_ps(ax + i);

		acc0 = _mm_add_ps(acc0, _mm_mul_ps(q, _mm_loadu_ps(bx0 + i)));
		acc1 = _mm_add_ps(acc1, _mm_mul_ps(q, _mm_loadu_ps(bx1 + i)));
		acc2 = _mm_add_ps(acc2, _mm_mul_ps(q, _mm_loadu_ps(bx2 + i)));
		acc3 = _mm_add_ps(acc3, _mm_mul_ps(q, _mm_loadu_ps(bx3 + i)));
	}

	distances[0] = -(double) hsum128_ps(acc0);
	distances[1] = -(double) hsum128_ps(acc1);
	distances[2] = -(double) hsum128_ps(acc2);
	distances[3] = -(double) hsum128_ps(acc3);

	for (; i < dim; i++)
	{
		float q = ax[i];
		distances[0] -= (double)(q * bx0[i]);
		distances[1] -= (double)(q * bx1[i]);
		distances[2] -= (double)(q * bx2[i]);
		distances[3] -= (double)(q * bx3[i]);
	}
}

static void __attribute__((target("sse2")))
VectorBatchNegativeInnerProduct_sse2(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	int k = 0;
	for (; k + 3 < count; k += 4)
		VectorBatchNegativeInnerProduct_sse2_4(dim, ax, bx + k, distances + k);
	for (; k < count; k++)
		distances[k] = -(double) VectorInnerProduct_sse2(dim, (float *) ax, (float *) bx[k]);
}
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
static void
VectorBatchNegativeInnerProduct_neon_4(int dim, const float *ax, const float * const *bx, double *distances)
{
	const float *bx0 = bx[0], *bx1 = bx[1], *bx2 = bx[2], *bx3 = bx[3];
	float32x4_t acc0 = vdupq_n_f32(0.0f);
	float32x4_t acc1 = vdupq_n_f32(0.0f);
	float32x4_t acc2 = vdupq_n_f32(0.0f);
	float32x4_t acc3 = vdupq_n_f32(0.0f);
	int i = 0;

	for (; i + 3 < dim; i += 4)
	{
		float32x4_t q = vld1q_f32(ax + i);

		acc0 = vmlaq_f32(acc0, q, vld1q_f32(bx0 + i));
		acc1 = vmlaq_f32(acc1, q, vld1q_f32(bx1 + i));
		acc2 = vmlaq_f32(acc2, q, vld1q_f32(bx2 + i));
		acc3 = vmlaq_f32(acc3, q, vld1q_f32(bx3 + i));
	}

	distances[0] = -(double) (vgetq_lane_f32(acc0, 0) + vgetq_lane_f32(acc0, 1) + vgetq_lane_f32(acc0, 2) + vgetq_lane_f32(acc0, 3));
	distances[1] = -(double) (vgetq_lane_f32(acc1, 0) + vgetq_lane_f32(acc1, 1) + vgetq_lane_f32(acc1, 2) + vgetq_lane_f32(acc1, 3));
	distances[2] = -(double) (vgetq_lane_f32(acc2, 0) + vgetq_lane_f32(acc2, 1) + vgetq_lane_f32(acc2, 2) + vgetq_lane_f32(acc2, 3));
	distances[3] = -(double) (vgetq_lane_f32(acc3, 0) + vgetq_lane_f32(acc3, 1) + vgetq_lane_f32(acc3, 2) + vgetq_lane_f32(acc3, 3));

	for (; i < dim; i++)
	{
		float q = ax[i];
		distances[0] -= (double)(q * bx0[i]);
		distances[1] -= (double)(q * bx1[i]);
		distances[2] -= (double)(q * bx2[i]);
		distances[3] -= (double)(q * bx3[i]);
	}
}

static void
VectorBatchNegativeInnerProduct_neon(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	int k = 0;
	for (; k + 3 < count; k += 4)
		VectorBatchNegativeInnerProduct_neon_4(dim, ax, bx + k, distances + k);
	for (; k < count; k++)
		distances[k] = -(double) VectorInnerProduct_neon(dim, (float *) ax, (float *) bx[k]);
}
#endif

static void
VectorBatchNegativeInnerProduct_scalar(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	for (int k = 0; k < count; k++)
	{
		float dist = 0.0;
		const float *b = bx[k];
		for (int i = 0; i < dim; i++)
		{
			dist += ax[i] * b[i];
		}
		distances[k] = -(double) dist;
	}
}

void
VectorBatchNegativeInnerProduct(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
#if defined(__x86_64__) || defined(__i386__)
	static void (*func) (int, const float *, const float * const *, double *, int) = NULL;

	if (func == NULL)
	{
		if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq"))
			func = VectorBatchNegativeInnerProduct_avx512f;
		else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
			func = VectorBatchNegativeInnerProduct_avx2;
		else if (__builtin_cpu_supports("sse2"))
			func = VectorBatchNegativeInnerProduct_sse2;
		else
			func = VectorBatchNegativeInnerProduct_scalar;
	}
	func(dim, ax, bx, distances, count);
#elif defined(__aarch64__) || defined(__ARM_NEON)
	VectorBatchNegativeInnerProduct_neon(dim, ax, bx, distances, count);
#else
	VectorBatchNegativeInnerProduct_scalar(dim, ax, bx, distances, count);
#endif
}

void
VectorBatchInnerProduct(int dim, const float *ax, const float * const *bx, double *distances, int count)
{
	VectorBatchNegativeInnerProduct(dim, ax, bx, distances, count);
	for (int i = 0; i < count; i++)
		distances[i] = -distances[i];
}

VectorBatchDistFunc
VectorGetBatchDistFunc(PGFunction fn)
{
	if (fn == (PGFunction) vector_l2_squared_distance)
		return VectorBatchL2SquaredDistance;
	if (fn == (PGFunction) l2_distance)
		return VectorBatchL2Distance;
	if (fn == (PGFunction) vector_negative_inner_product)
		return VectorBatchNegativeInnerProduct;
	if (fn == (PGFunction) inner_product)
		return VectorBatchInnerProduct;
	return NULL;
}

#if defined(__x86_64__) || defined(__i386__)
static void __attribute__((target("avx512f,avx512dq")))
VectorBatchL2SquaredDistance_SoA_avx512f(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	float res[16];
	__m512 acc = _mm512_setzero_ps();
	for (int d = 0; d < dim; d++)
	{
		__m512 q_val = _mm512_set1_ps(q[d]);
		__m512 v = _mm512_loadu_ps(soa_bx + d * 16);
		__m512 diff = _mm512_sub_ps(q_val, v);
		acc = _mm512_fmadd_ps(diff, diff, acc);
	}
	_mm512_storeu_ps(res, acc);
	for (int i = 0; i < count; i++)
		distances[i] = (double) res[i];
}

static void __attribute__((target("avx2,fma")))
VectorBatchL2SquaredDistance_SoA_avx2(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	float res[16];
	__m256 acc0 = _mm256_setzero_ps();
	__m256 acc1 = _mm256_setzero_ps();
	for (int d = 0; d < dim; d++)
	{
		__m256 q_val = _mm256_set1_ps(q[d]);
		const float *slice = soa_bx + d * 16;
		__m256 diff0 = _mm256_sub_ps(q_val, _mm256_loadu_ps(slice));
		__m256 diff1 = _mm256_sub_ps(q_val, _mm256_loadu_ps(slice + 8));
		acc0 = _mm256_fmadd_ps(diff0, diff0, acc0);
		acc1 = _mm256_fmadd_ps(diff1, diff1, acc1);
	}
	_mm256_storeu_ps(res, acc0);
	_mm256_storeu_ps(res + 8, acc1);
	for (int i = 0; i < count; i++)
		distances[i] = (double) res[i];
}

static void __attribute__((target("sse2")))
VectorBatchL2SquaredDistance_SoA_sse2(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	float res[16];
	__m128 acc0 = _mm_setzero_ps();
	__m128 acc1 = _mm_setzero_ps();
	__m128 acc2 = _mm_setzero_ps();
	__m128 acc3 = _mm_setzero_ps();
	for (int d = 0; d < dim; d++)
	{
		__m128 q_val = _mm_set1_ps(q[d]);
		const float *slice = soa_bx + d * 16;
		__m128 diff0 = _mm_sub_ps(q_val, _mm_loadu_ps(slice));
		__m128 diff1 = _mm_sub_ps(q_val, _mm_loadu_ps(slice + 4));
		__m128 diff2 = _mm_sub_ps(q_val, _mm_loadu_ps(slice + 8));
		__m128 diff3 = _mm_sub_ps(q_val, _mm_loadu_ps(slice + 12));
		acc0 = _mm_add_ps(acc0, _mm_mul_ps(diff0, diff0));
		acc1 = _mm_add_ps(acc1, _mm_mul_ps(diff1, diff1));
		acc2 = _mm_add_ps(acc2, _mm_mul_ps(diff2, diff2));
		acc3 = _mm_add_ps(acc3, _mm_mul_ps(diff3, diff3));
	}
	_mm_storeu_ps(res, acc0);
	_mm_storeu_ps(res + 4, acc1);
	_mm_storeu_ps(res + 8, acc2);
	_mm_storeu_ps(res + 12, acc3);
	for (int i = 0; i < count; i++)
		distances[i] = (double) res[i];
}

static void __attribute__((target("avx512f,avx512dq")))
VectorBatchNegativeInnerProduct_SoA_avx512f(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	float res[16];
	__m512 acc = _mm512_setzero_ps();
	for (int d = 0; d < dim; d++)
	{
		__m512 q_val = _mm512_set1_ps(q[d]);
		__m512 v = _mm512_loadu_ps(soa_bx + d * 16);
		acc = _mm512_fmadd_ps(q_val, v, acc);
	}
	_mm512_storeu_ps(res, acc);
	for (int i = 0; i < count; i++)
		distances[i] = -(double) res[i];
}

static void __attribute__((target("avx2,fma")))
VectorBatchNegativeInnerProduct_SoA_avx2(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	float res[16];
	__m256 acc0 = _mm256_setzero_ps();
	__m256 acc1 = _mm256_setzero_ps();
	for (int d = 0; d < dim; d++)
	{
		__m256 q_val = _mm256_set1_ps(q[d]);
		const float *slice = soa_bx + d * 16;
		acc0 = _mm256_fmadd_ps(q_val, _mm256_loadu_ps(slice), acc0);
		acc1 = _mm256_fmadd_ps(q_val, _mm256_loadu_ps(slice + 8), acc1);
	}
	_mm256_storeu_ps(res, acc0);
	_mm256_storeu_ps(res + 8, acc1);
	for (int i = 0; i < count; i++)
		distances[i] = -(double) res[i];
}

static void __attribute__((target("sse2")))
VectorBatchNegativeInnerProduct_SoA_sse2(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	float res[16];
	__m128 acc0 = _mm_setzero_ps();
	__m128 acc1 = _mm_setzero_ps();
	__m128 acc2 = _mm_setzero_ps();
	__m128 acc3 = _mm_setzero_ps();
	for (int d = 0; d < dim; d++)
	{
		__m128 q_val = _mm_set1_ps(q[d]);
		const float *slice = soa_bx + d * 16;
		acc0 = _mm_add_ps(acc0, _mm_mul_ps(q_val, _mm_loadu_ps(slice)));
		acc1 = _mm_add_ps(acc1, _mm_mul_ps(q_val, _mm_loadu_ps(slice + 4)));
		acc2 = _mm_add_ps(acc2, _mm_mul_ps(q_val, _mm_loadu_ps(slice + 8)));
		acc3 = _mm_add_ps(acc3, _mm_mul_ps(q_val, _mm_loadu_ps(slice + 12)));
	}
	_mm_storeu_ps(res, acc0);
	_mm_storeu_ps(res + 4, acc1);
	_mm_storeu_ps(res + 8, acc2);
	_mm_storeu_ps(res + 12, acc3);
	for (int i = 0; i < count; i++)
		distances[i] = -(double) res[i];
}
#endif

static void
VectorBatchL2SquaredDistance_SoA_scalar(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	double acc[16] = {0};
	for (int d = 0; d < dim; d++)
	{
		float q_val = q[d];
		const float *slice = soa_bx + d * 16;
		for (int i = 0; i < count; i++)
		{
			float diff = q_val - slice[i];
			acc[i] += (double) (diff * diff);
		}
	}
	for (int i = 0; i < count; i++)
		distances[i] = acc[i];
}

static void
VectorBatchNegativeInnerProduct_SoA_scalar(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	double acc[16] = {0};
	for (int d = 0; d < dim; d++)
	{
		float q_val = q[d];
		const float *slice = soa_bx + d * 16;
		for (int i = 0; i < count; i++)
			acc[i] += (double) (q_val * slice[i]);
	}
	for (int i = 0; i < count; i++)
		distances[i] = -acc[i];
}

void
VectorBatchL2SquaredDistance_SoA(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
#if defined(__x86_64__) || defined(__i386__)
	if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq"))
		VectorBatchL2SquaredDistance_SoA_avx512f(dim, q, soa_bx, distances, count);
	else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
		VectorBatchL2SquaredDistance_SoA_avx2(dim, q, soa_bx, distances, count);
	else if (__builtin_cpu_supports("sse2"))
		VectorBatchL2SquaredDistance_SoA_sse2(dim, q, soa_bx, distances, count);
	else
		VectorBatchL2SquaredDistance_SoA_scalar(dim, q, soa_bx, distances, count);
#else
	VectorBatchL2SquaredDistance_SoA_scalar(dim, q, soa_bx, distances, count);
#endif
}

void
VectorBatchNegativeInnerProduct_SoA(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
#if defined(__x86_64__) || defined(__i386__)
	if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq"))
		VectorBatchNegativeInnerProduct_SoA_avx512f(dim, q, soa_bx, distances, count);
	else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
		VectorBatchNegativeInnerProduct_SoA_avx2(dim, q, soa_bx, distances, count);
	else if (__builtin_cpu_supports("sse2"))
		VectorBatchNegativeInnerProduct_SoA_sse2(dim, q, soa_bx, distances, count);
	else
		VectorBatchNegativeInnerProduct_SoA_scalar(dim, q, soa_bx, distances, count);
#else
	VectorBatchNegativeInnerProduct_SoA_scalar(dim, q, soa_bx, distances, count);
#endif
}

void
VectorBatchL2Distance_SoA(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	VectorBatchL2SquaredDistance_SoA(dim, q, soa_bx, distances, count);
	for (int i = 0; i < count; i++)
		distances[i] = sqrt(distances[i]);
}

void
VectorBatchInnerProduct_SoA(int dim, const float *q, const float *soa_bx, double *distances, int count)
{
	VectorBatchNegativeInnerProduct_SoA(dim, q, soa_bx, distances, count);
	for (int i = 0; i < count; i++)
		distances[i] = -distances[i];
}

VectorSoABatchDistFunc
VectorGetSoABatchDistFunc(PGFunction fn)
{
	if (fn == (PGFunction) vector_l2_squared_distance)
		return VectorBatchL2SquaredDistance_SoA;
	if (fn == (PGFunction) l2_distance)
		return VectorBatchL2Distance_SoA;
	if (fn == (PGFunction) vector_negative_inner_product)
		return VectorBatchNegativeInnerProduct_SoA;
	if (fn == (PGFunction) inner_product)
		return VectorBatchInnerProduct_SoA;
	return NULL;
}

#if defined(__x86_64__) || defined(__i386__)
static const int32_t soa_masks[8][8] __attribute__((aligned(32))) = {
	{0, 0, 0, 0, 0, 0, 0, 0},
	{-1, 0, 0, 0, 0, 0, 0, 0},
	{-1, -1, 0, 0, 0, 0, 0, 0},
	{-1, -1, -1, 0, 0, 0, 0, 0},
	{-1, -1, -1, -1, 0, 0, 0, 0},
	{-1, -1, -1, -1, -1, 0, 0, 0},
	{-1, -1, -1, -1, -1, -1, 0, 0},
	{-1, -1, -1, -1, -1, -1, -1, 0}
};
#endif

void
VectorBatchL2SquaredDistance_SoA_InPlace(int dim, const float *q, const float *soa_values, double *distances, int count)
{
	int k = 0;
#if defined(__x86_64__) || defined(__i386__)
	if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
	{
		for (; k + 7 < count; k += 8)
		{
			float res[8];
			__m256 acc0 = _mm256_setzero_ps();
			__m256 acc1 = _mm256_setzero_ps();
			__m256 acc2 = _mm256_setzero_ps();
			__m256 acc3 = _mm256_setzero_ps();
			int d = 0;

			for (; d + 3 < dim; d += 4)
			{
				__m256 q0 = _mm256_set1_ps(q[d + 0]);
				__m256 q1 = _mm256_set1_ps(q[d + 1]);
				__m256 q2 = _mm256_set1_ps(q[d + 2]);
				__m256 q3 = _mm256_set1_ps(q[d + 3]);

				const float *s0 = soa_values + (d + 0) * count + k;
				const float *s1 = soa_values + (d + 1) * count + k;
				const float *s2 = soa_values + (d + 2) * count + k;
				const float *s3 = soa_values + (d + 3) * count + k;

				__m256 diff0 = _mm256_sub_ps(q0, _mm256_loadu_ps(s0));
				__m256 diff1 = _mm256_sub_ps(q1, _mm256_loadu_ps(s1));
				__m256 diff2 = _mm256_sub_ps(q2, _mm256_loadu_ps(s2));
				__m256 diff3 = _mm256_sub_ps(q3, _mm256_loadu_ps(s3));

				acc0 = _mm256_fmadd_ps(diff0, diff0, acc0);
				acc1 = _mm256_fmadd_ps(diff1, diff1, acc1);
				acc2 = _mm256_fmadd_ps(diff2, diff2, acc2);
				acc3 = _mm256_fmadd_ps(diff3, diff3, acc3);
			}

			acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));

			for (; d < dim; d++)
			{
				__m256 q_val = _mm256_set1_ps(q[d]);
				__m256 v = _mm256_loadu_ps(soa_values + d * count + k);
				__m256 diff = _mm256_sub_ps(q_val, v);
				acc0 = _mm256_fmadd_ps(diff, diff, acc0);
			}

			_mm256_storeu_ps(res, acc0);
			for (int i = 0; i < 8; i++)
				distances[k + i] = (double) res[i];
		}

		int rem = count - k;
		if (rem > 0)
		{
			__m256i mask = _mm256_load_si256((const __m256i *) soa_masks[rem]);
			float res[8];
			__m256 acc0 = _mm256_setzero_ps();
			__m256 acc1 = _mm256_setzero_ps();
			__m256 acc2 = _mm256_setzero_ps();
			__m256 acc3 = _mm256_setzero_ps();
			int d = 0;

			for (; d + 3 < dim; d += 4)
			{
				__m256 q0 = _mm256_set1_ps(q[d + 0]);
				__m256 q1 = _mm256_set1_ps(q[d + 1]);
				__m256 q2 = _mm256_set1_ps(q[d + 2]);
				__m256 q3 = _mm256_set1_ps(q[d + 3]);

				__m256 v0 = _mm256_maskload_ps(soa_values + (d + 0) * count + k, mask);
				__m256 v1 = _mm256_maskload_ps(soa_values + (d + 1) * count + k, mask);
				__m256 v2 = _mm256_maskload_ps(soa_values + (d + 2) * count + k, mask);
				__m256 v3 = _mm256_maskload_ps(soa_values + (d + 3) * count + k, mask);

				__m256 diff0 = _mm256_sub_ps(q0, v0);
				__m256 diff1 = _mm256_sub_ps(q1, v1);
				__m256 diff2 = _mm256_sub_ps(q2, v2);
				__m256 diff3 = _mm256_sub_ps(q3, v3);

				acc0 = _mm256_fmadd_ps(diff0, diff0, acc0);
				acc1 = _mm256_fmadd_ps(diff1, diff1, acc1);
				acc2 = _mm256_fmadd_ps(diff2, diff2, acc2);
				acc3 = _mm256_fmadd_ps(diff3, diff3, acc3);
			}

			acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));

			for (; d < dim; d++)
			{
				__m256 q_val = _mm256_set1_ps(q[d]);
				__m256 v = _mm256_maskload_ps(soa_values + d * count + k, mask);
				__m256 diff = _mm256_sub_ps(q_val, v);
				acc0 = _mm256_fmadd_ps(diff, diff, acc0);
			}

			_mm256_storeu_ps(res, acc0);
			for (int i = 0; i < rem; i++)
				distances[k + i] = (double) res[i];
			k = count;
		}
	}
#endif
	for (; k < count; k++)
	{
		double dist = 0.0;
		for (int d = 0; d < dim; d++)
		{
			float diff = q[d] - soa_values[d * count + k];
			dist += (double) (diff * diff);
		}
		distances[k] = dist;
	}
}

void
VectorBatchNegativeInnerProduct_SoA_InPlace(int dim, const float *q, const float *soa_values, double *distances, int count)
{
	int k = 0;
#if defined(__x86_64__) || defined(__i386__)
	if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
	{
		for (; k + 7 < count; k += 8)
		{
			float res[8];
			__m256 acc0 = _mm256_setzero_ps();
			__m256 acc1 = _mm256_setzero_ps();
			__m256 acc2 = _mm256_setzero_ps();
			__m256 acc3 = _mm256_setzero_ps();
			int d = 0;

			for (; d + 3 < dim; d += 4)
			{
				__m256 q0 = _mm256_set1_ps(q[d + 0]);
				__m256 q1 = _mm256_set1_ps(q[d + 1]);
				__m256 q2 = _mm256_set1_ps(q[d + 2]);
				__m256 q3 = _mm256_set1_ps(q[d + 3]);

				const float *s0 = soa_values + (d + 0) * count + k;
				const float *s1 = soa_values + (d + 1) * count + k;
				const float *s2 = soa_values + (d + 2) * count + k;
				const float *s3 = soa_values + (d + 3) * count + k;

				acc0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(s0), acc0);
				acc1 = _mm256_fmadd_ps(q1, _mm256_loadu_ps(s1), acc1);
				acc2 = _mm256_fmadd_ps(q2, _mm256_loadu_ps(s2), acc2);
				acc3 = _mm256_fmadd_ps(q3, _mm256_loadu_ps(s3), acc3);
			}

			acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));

			for (; d < dim; d++)
			{
				__m256 q_val = _mm256_set1_ps(q[d]);
				__m256 v = _mm256_loadu_ps(soa_values + d * count + k);
				acc0 = _mm256_fmadd_ps(q_val, v, acc0);
			}

			_mm256_storeu_ps(res, acc0);
			for (int i = 0; i < 8; i++)
				distances[k + i] = -(double) res[i];
		}

		int rem = count - k;
		if (rem > 0)
		{
			__m256i mask = _mm256_load_si256((const __m256i *) soa_masks[rem]);
			float res[8];
			__m256 acc0 = _mm256_setzero_ps();
			__m256 acc1 = _mm256_setzero_ps();
			__m256 acc2 = _mm256_setzero_ps();
			__m256 acc3 = _mm256_setzero_ps();
			int d = 0;

			for (; d + 3 < dim; d += 4)
			{
				__m256 q0 = _mm256_set1_ps(q[d + 0]);
				__m256 q1 = _mm256_set1_ps(q[d + 1]);
				__m256 q2 = _mm256_set1_ps(q[d + 2]);
				__m256 q3 = _mm256_set1_ps(q[d + 3]);

				__m256 v0 = _mm256_maskload_ps(soa_values + (d + 0) * count + k, mask);
				__m256 v1 = _mm256_maskload_ps(soa_values + (d + 1) * count + k, mask);
				__m256 v2 = _mm256_maskload_ps(soa_values + (d + 2) * count + k, mask);
				__m256 v3 = _mm256_maskload_ps(soa_values + (d + 3) * count + k, mask);

				acc0 = _mm256_fmadd_ps(q0, v0, acc0);
				acc1 = _mm256_fmadd_ps(q1, v1, acc1);
				acc2 = _mm256_fmadd_ps(q2, v2, acc2);
				acc3 = _mm256_fmadd_ps(q3, v3, acc3);
			}

			acc0 = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));

			for (; d < dim; d++)
			{
				__m256 q_val = _mm256_set1_ps(q[d]);
				__m256 v = _mm256_maskload_ps(soa_values + d * count + k, mask);
				acc0 = _mm256_fmadd_ps(q_val, v, acc0);
			}

			_mm256_storeu_ps(res, acc0);
			for (int i = 0; i < rem; i++)
				distances[k + i] = -(double) res[i];
			k = count;
		}
	}
#endif
	for (; k < count; k++)
	{
		double dist = 0.0;
		for (int d = 0; d < dim; d++)
		{
			dist += (double) (q[d] * soa_values[d * count + k]);
		}
		distances[k] = -dist;
	}
}

VectorSoABatchDistFunc_InPlace
VectorGetSoABatchDistFunc_InPlace(PGFunction fn)
{
	if (fn == (PGFunction) vector_l2_squared_distance || fn == (PGFunction) l2_distance)
		return VectorBatchL2SquaredDistance_SoA_InPlace;
	if (fn == (PGFunction) vector_negative_inner_product || fn == (PGFunction) inner_product)
		return VectorBatchNegativeInnerProduct_SoA_InPlace;
	return NULL;
}

/*
 * Cosine similarity kernels. Runtime dispatch:
 *   x86: AVX512F -> AVX2+FMA -> SSE2 -> scalar (auto-vectorized)
 *   ARM: NEON (aarch64 / ARMv7+NEON)
 *   other: scalar (auto-vectorized)
 * Computes 3 dot products (similarity, norma, normb) in one pass.
 */
#if defined(__x86_64__) || defined(__i386__)
static double __attribute__((target("avx512f,avx512dq")))
VectorCosineSimilarity_avx512f(int dim, float *ax, float *bx)
{
	float		similarity = 0.0;
	float		norma = 0.0;
	float		normb = 0.0;
	int			i = 0;
	__m512		s0 = _mm512_setzero_ps(), s1 = s0;
	__m512		na0 = _mm512_setzero_ps(), na1 = na0;
	__m512		nb0 = _mm512_setzero_ps(), nb1 = nb0;
	__m512		acc;
	__m256		lo, hi, s256;
	__m128		s128;

	for (; i + 31 < dim; i += 32)
	{
		__m512		a = _mm512_loadu_ps(ax + i);
		__m512		b = _mm512_loadu_ps(bx + i);
		__m512		c = _mm512_loadu_ps(ax + i + 16);
		__m512		d = _mm512_loadu_ps(bx + i + 16);

		s0 = _mm512_fmadd_ps(a, b, s0);
		s1 = _mm512_fmadd_ps(c, d, s1);
		na0 = _mm512_fmadd_ps(a, a, na0);
		na1 = _mm512_fmadd_ps(c, c, na1);
		nb0 = _mm512_fmadd_ps(b, b, nb0);
		nb1 = _mm512_fmadd_ps(d, d, nb1);
	}
	for (; i + 15 < dim; i += 16)
	{
		__m512		a = _mm512_loadu_ps(ax + i);
		__m512		b = _mm512_loadu_ps(bx + i);

		s0 = _mm512_fmadd_ps(a, b, s0);
		na0 = _mm512_fmadd_ps(a, a, na0);
		nb0 = _mm512_fmadd_ps(b, b, nb0);
	}
	for (; i < dim; i++)
	{
		similarity += ax[i] * bx[i];
		norma += ax[i] * ax[i];
		normb += bx[i] * bx[i];
	}

	acc = _mm512_add_ps(s0, s1);
	lo = _mm512_castps512_ps256(acc);
	hi = _mm512_extractf32x8_ps(acc, 1);
	s256 = _mm256_add_ps(lo, hi);
	s128 = _mm_add_ps(_mm256_castps256_ps128(s256), _mm256_extractf128_ps(s256, 1));
	s128 = _mm_hadd_ps(s128, s128);
	s128 = _mm_hadd_ps(s128, s128);
	similarity += _mm_cvtss_f32(s128);

	acc = _mm512_add_ps(na0, na1);
	lo = _mm512_castps512_ps256(acc);
	hi = _mm512_extractf32x8_ps(acc, 1);
	s256 = _mm256_add_ps(lo, hi);
	s128 = _mm_add_ps(_mm256_castps256_ps128(s256), _mm256_extractf128_ps(s256, 1));
	s128 = _mm_hadd_ps(s128, s128);
	s128 = _mm_hadd_ps(s128, s128);
	norma += _mm_cvtss_f32(s128);

	acc = _mm512_add_ps(nb0, nb1);
	lo = _mm512_castps512_ps256(acc);
	hi = _mm512_extractf32x8_ps(acc, 1);
	s256 = _mm256_add_ps(lo, hi);
	s128 = _mm_add_ps(_mm256_castps256_ps128(s256), _mm256_extractf128_ps(s256, 1));
	s128 = _mm_hadd_ps(s128, s128);
	s128 = _mm_hadd_ps(s128, s128);
	normb += _mm_cvtss_f32(s128);

	return (double) similarity / sqrt((double) norma * (double) normb);
}

static double __attribute__((target("avx2,fma")))
VectorCosineSimilarity_avx2(int dim, float *ax, float *bx)
{
	float		similarity = 0.0;
	float		norma = 0.0;
	float		normb = 0.0;
	int			i = 0;
	__m256		s0 = _mm256_setzero_ps(), s1 = s0;
	__m256		na0 = _mm256_setzero_ps(), na1 = na0;
	__m256		nb0 = _mm256_setzero_ps(), nb1 = nb0;
	__m256		acc;
	__m128		lo, hi, s;

	for (; i + 15 < dim; i += 16)
	{
		__m256		a = _mm256_loadu_ps(ax + i);
		__m256		b = _mm256_loadu_ps(bx + i);
		__m256		c = _mm256_loadu_ps(ax + i + 8);
		__m256		d = _mm256_loadu_ps(bx + i + 8);

		s0 = _mm256_fmadd_ps(a, b, s0);
		s1 = _mm256_fmadd_ps(c, d, s1);
		na0 = _mm256_fmadd_ps(a, a, na0);
		na1 = _mm256_fmadd_ps(c, c, na1);
		nb0 = _mm256_fmadd_ps(b, b, nb0);
		nb1 = _mm256_fmadd_ps(d, d, nb1);
	}
	for (; i + 7 < dim; i += 8)
	{
		__m256		a = _mm256_loadu_ps(ax + i);
		__m256		b = _mm256_loadu_ps(bx + i);

		s0 = _mm256_fmadd_ps(a, b, s0);
		na0 = _mm256_fmadd_ps(a, a, na0);
		nb0 = _mm256_fmadd_ps(b, b, nb0);
	}
	for (; i < dim; i++)
	{
		similarity += ax[i] * bx[i];
		norma += ax[i] * ax[i];
		normb += bx[i] * bx[i];
	}

	acc = _mm256_add_ps(s0, s1);
	lo = _mm256_castps256_ps128(acc);
	hi = _mm256_extractf128_ps(acc, 1);
	s = _mm_add_ps(lo, hi);
	s = _mm_hadd_ps(s, s);
	s = _mm_hadd_ps(s, s);
	similarity += _mm_cvtss_f32(s);

	acc = _mm256_add_ps(na0, na1);
	lo = _mm256_castps256_ps128(acc);
	hi = _mm256_extractf128_ps(acc, 1);
	s = _mm_add_ps(lo, hi);
	s = _mm_hadd_ps(s, s);
	s = _mm_hadd_ps(s, s);
	norma += _mm_cvtss_f32(s);

	acc = _mm256_add_ps(nb0, nb1);
	lo = _mm256_castps256_ps128(acc);
	hi = _mm256_extractf128_ps(acc, 1);
	s = _mm_add_ps(lo, hi);
	s = _mm_hadd_ps(s, s);
	s = _mm_hadd_ps(s, s);
	normb += _mm_cvtss_f32(s);

	return (double) similarity / sqrt((double) norma * (double) normb);
}

static double __attribute__((target("sse2")))
VectorCosineSimilarity_sse2(int dim, float *ax, float *bx)
{
	float		similarity = 0.0;
	float		norma = 0.0;
	float		normb = 0.0;
	int			i = 0;
	__m128		s0 = _mm_setzero_ps();
	__m128		na0 = _mm_setzero_ps();
	__m128		nb0 = _mm_setzero_ps();
	__m128		s, hi;

	for (; i + 3 < dim; i += 4)
	{
		__m128		a = _mm_loadu_ps(ax + i);
		__m128		b = _mm_loadu_ps(bx + i);

		s0 = _mm_add_ps(_mm_mul_ps(a, b), s0);
		na0 = _mm_add_ps(_mm_mul_ps(a, a), na0);
		nb0 = _mm_add_ps(_mm_mul_ps(b, b), nb0);
	}
	for (; i < dim; i++)
	{
		similarity += ax[i] * bx[i];
		norma += ax[i] * ax[i];
		normb += bx[i] * bx[i];
	}

	s = s0;
	hi = _mm_movehl_ps(s, s);
	s = _mm_add_ps(s, hi);
	s = _mm_add_ss(s, _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1)));
	similarity += _mm_cvtss_f32(s);

	s = na0;
	hi = _mm_movehl_ps(s, s);
	s = _mm_add_ps(s, hi);
	s = _mm_add_ss(s, _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1)));
	norma += _mm_cvtss_f32(s);

	s = nb0;
	hi = _mm_movehl_ps(s, s);
	s = _mm_add_ps(s, hi);
	s = _mm_add_ss(s, _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1)));
	normb += _mm_cvtss_f32(s);

	return (double) similarity / sqrt((double) norma * (double) normb);
}
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
static double
VectorCosineSimilarity_neon(int dim, float *ax, float *bx)
{
	float		similarity = 0.0;
	float		norma = 0.0;
	float		normb = 0.0;
	int			i = 0;
	float32x4_t	s0 = vdupq_n_f32(0.0f), s1 = s0;
	float32x4_t	na0 = vdupq_n_f32(0.0f), na1 = na0;
	float32x4_t	nb0 = vdupq_n_f32(0.0f), nb1 = nb0;
	float32x4_t	sum;
	float32x2_t	lo, hi, sum2;

	for (; i + 7 < dim; i += 8)
	{
		float32x4_t a = vld1q_f32(ax + i);
		float32x4_t b = vld1q_f32(bx + i);
		float32x4_t c = vld1q_f32(ax + i + 4);
		float32x4_t d = vld1q_f32(bx + i + 4);

		s0 = vmlaq_f32(s0, a, b);
		s1 = vmlaq_f32(s1, c, d);
		na0 = vmlaq_f32(na0, a, a);
		na1 = vmlaq_f32(na1, c, c);
		nb0 = vmlaq_f32(nb0, b, b);
		nb1 = vmlaq_f32(nb1, d, d);
	}
	for (; i + 3 < dim; i += 4)
	{
		float32x4_t a = vld1q_f32(ax + i);
		float32x4_t b = vld1q_f32(bx + i);

		s0 = vmlaq_f32(s0, a, b);
		na0 = vmlaq_f32(na0, a, a);
		nb0 = vmlaq_f32(nb0, b, b);
	}
	for (; i < dim; i++)
	{
		similarity += ax[i] * bx[i];
		norma += ax[i] * ax[i];
		normb += bx[i] * bx[i];
	}

	sum = vaddq_f32(s0, s1);
	lo = vget_low_f32(sum);
	hi = vget_high_f32(sum);
	sum2 = vadd_f32(lo, hi);
	sum2 = vpadd_f32(sum2, sum2);
	similarity += vget_lane_f32(sum2, 0);

	sum = vaddq_f32(na0, na1);
	lo = vget_low_f32(sum);
	hi = vget_high_f32(sum);
	sum2 = vadd_f32(lo, hi);
	sum2 = vpadd_f32(sum2, sum2);
	norma += vget_lane_f32(sum2, 0);

	sum = vaddq_f32(nb0, nb1);
	lo = vget_low_f32(sum);
	hi = vget_high_f32(sum);
	sum2 = vadd_f32(lo, hi);
	sum2 = vpadd_f32(sum2, sum2);
	normb += vget_lane_f32(sum2, 0);

	return (double) similarity / sqrt((double) norma * (double) normb);
}
#endif

static double
VectorCosineSimilarity_scalar(int dim, float *ax, float *bx)
{
	float		similarity = 0.0;
	float		norma = 0.0;
	float		normb = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < dim; i++)
	{
		similarity += ax[i] * bx[i];
		norma += ax[i] * ax[i];
		normb += bx[i] * bx[i];
	}

	/* Use sqrt(a * b) over sqrt(a) * sqrt(b) */
	return (double) similarity / sqrt((double) norma * (double) normb);
}

static double
VectorCosineSimilarity(int dim, float *ax, float *bx)
{
#if defined(__x86_64__) || defined(__i386__)
	static double (*func) (int, float *, float *) = NULL;

	if (func == NULL)
	{
		if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq"))
			func = VectorCosineSimilarity_avx512f;
		else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
			func = VectorCosineSimilarity_avx2;
		else if (__builtin_cpu_supports("sse2"))
			func = VectorCosineSimilarity_sse2;
		else
			func = VectorCosineSimilarity_scalar;
	}
	return func(dim, ax, bx);
#elif defined(__aarch64__) || defined(__ARM_NEON)
	return VectorCosineSimilarity_neon(dim, ax, bx);
#else
	return VectorCosineSimilarity_scalar(dim, ax, bx);
#endif
}

/*
 * Get the cosine distance between two vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(cosine_distance);
Datum
cosine_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	double		similarity;

	CheckDims(a, b);

	similarity = VectorCosineSimilarity(a->dim, a->x, b->x);

#ifdef _MSC_VER
	/* /fp:fast may not propagate NaN */
	if (isnan(similarity))
		PG_RETURN_FLOAT8(NAN);
#endif

	/* Keep in range */
	if (similarity > 1)
		similarity = 1.0;
	else if (similarity < -1)
		similarity = -1.0;

	PG_RETURN_FLOAT8(1.0 - similarity);
}

/*
 * Get the distance for spherical k-means
 * Currently uses angular distance since needs to satisfy triangle inequality
 * Assumes inputs are unit vectors (skips norm)
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_spherical_distance);
Datum
vector_spherical_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	double		distance;

	CheckDims(a, b);

	distance = (double) VectorInnerProduct(a->dim, a->x, b->x);

	/* Prevent NaN with acos with loss of precision */
	if (distance > 1)
		distance = 1;
	else if (distance < -1)
		distance = -1;

	PG_RETURN_FLOAT8(acos(distance) / M_PI);
}

/* Does not require FMA, but keep logic simple */
VECTOR_TARGET_CLONES static float
VectorL1Distance(int dim, float *ax, float *bx)
{
	float		distance = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < dim; i++)
		distance += fabsf(ax[i] - bx[i]);

	return distance;
}

/*
 * Get the L1 distance between two vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l1_distance);
Datum
l1_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double) VectorL1Distance(a->dim, a->x, b->x));
}

/*
 * Get the dimensions of a vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_dims);
Datum
vector_dims(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);

	PG_RETURN_INT32(a->dim);
}

/*
 * Get the L2 norm of a vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_norm);
Datum
vector_norm(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	float	   *ax = a->x;
	double		norm = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < a->dim; i++)
		norm += (double) ax[i] * (double) ax[i];

	PG_RETURN_FLOAT8(sqrt(norm));
}

/*
 * Normalize a vector with the L2 norm
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l2_normalize);
Datum
l2_normalize(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	float	   *ax = a->x;
	double		norm = 0;
	Vector	   *result;
	float	   *rx;

	result = InitVector(a->dim);
	rx = result->x;

	/* Auto-vectorized */
	for (int i = 0; i < a->dim; i++)
		norm += (double) ax[i] * (double) ax[i];

	norm = sqrt(norm);

	/* Return zero vector for zero norm */
	if (norm > 0)
	{
		for (int i = 0; i < a->dim; i++)
			rx[i] = (float) (ax[i] / norm);

		/* Check for overflow */
		for (int i = 0; i < a->dim; i++)
		{
			if (isinf(rx[i]))
				float_overflow_error();
		}
	}

	PG_RETURN_POINTER(result);
}

/*
 * Add vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_add);
Datum
vector_add(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	float	   *ax = a->x;
	float	   *bx = b->x;
	Vector	   *result;
	float	   *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	/* Auto-vectorized */
	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] + bx[i];

	/* Check for overflow */
	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * Subtract vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_sub);
Datum
vector_sub(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	float	   *ax = a->x;
	float	   *bx = b->x;
	Vector	   *result;
	float	   *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	/* Auto-vectorized */
	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] - bx[i];

	/* Check for overflow */
	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * Multiply vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_mul);
Datum
vector_mul(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	float	   *ax = a->x;
	float	   *bx = b->x;
	Vector	   *result;
	float	   *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	/* Auto-vectorized */
	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] * bx[i];

	/* Check for overflow and underflow */
	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();

		if (rx[i] == 0 && !(ax[i] == 0 || bx[i] == 0))
			float_underflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * Concatenate vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_concat);
Datum
vector_concat(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	Vector	   *result;
	int			dim = a->dim + b->dim;

	CheckDim(dim);
	result = InitVector(dim);

	/* Auto-vectorized */
	for (int i = 0, imax = a->dim; i < imax; i++)
		result->x[i] = a->x[i];

	/* Auto-vectorized */
	for (int i = 0, imax = b->dim, start = a->dim; i < imax; i++)
		result->x[i + start] = b->x[i];

	PG_RETURN_POINTER(result);
}

/*
 * Quantize a vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(binary_quantize);
Datum
binary_quantize(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	float	   *ax = a->x;
	VarBit	   *result = InitBitVector(a->dim);
	unsigned char *rx = VARBITS(result);
	int			i = 0;
	int			count = (a->dim / 8) * 8;

	/* Auto-vectorized */
	for (; i < count; i += 8)
	{
		unsigned char result_byte = 0;

		for (int j = 0; j < 8; j++)
			result_byte |= (ax[i + j] > 0) << (7 - j);

		rx[i / 8] = result_byte;
	}

	for (; i < a->dim; i++)
		rx[i / 8] |= (ax[i] > 0) << (7 - (i % 8));

	PG_RETURN_VARBIT_P(result);
}

/*
 * Get a subvector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(subvector);
Datum
subvector(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	int32		start = PG_GETARG_INT32(1);
	int32		count = PG_GETARG_INT32(2);
	int32		end;
	float	   *ax = a->x;
	Vector	   *result;
	int			dim;

	if (count < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	/*
	 * Check if (start + count > a->dim), avoiding integer overflow. a->dim
	 * and count are both positive, so a->dim - count won't overflow.
	 */
	if (start > a->dim - count)
		end = a->dim + 1;
	else
		end = start + count;

	/* Indexing starts at 1, like substring */
	if (start < 1)
		start = 1;
	else if (start > a->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	dim = end - start;
	CheckDim(dim);
	result = InitVector(dim);

	for (int i = 0; i < dim; i++)
		result->x[i] = ax[start - 1 + i];

	PG_RETURN_POINTER(result);
}

/*
 * Internal helper to compare vectors
 */
int
vector_cmp_internal(Vector * a, Vector * b)
{
	int			dim = Min(a->dim, b->dim);

	/* Check values before dimensions to be consistent with Postgres arrays */
	for (int i = 0; i < dim; i++)
	{
		if (a->x[i] < b->x[i])
			return -1;

		if (a->x[i] > b->x[i])
			return 1;
	}

	if (a->dim < b->dim)
		return -1;

	if (a->dim > b->dim)
		return 1;

	return 0;
}

/*
 * Less than
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_lt);
Datum
vector_lt(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) < 0);
}

/*
 * Less than or equal
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_le);
Datum
vector_le(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) <= 0);
}

/*
 * Equal
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_eq);
Datum
vector_eq(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) == 0);
}

/*
 * Not equal
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_ne);
Datum
vector_ne(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) != 0);
}

/*
 * Greater than or equal
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_ge);
Datum
vector_ge(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) >= 0);
}

/*
 * Greater than
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_gt);
Datum
vector_gt(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) > 0);
}

/*
 * Compare vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_cmp);
Datum
vector_cmp(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_INT32(vector_cmp_internal(a, b));
}

/*
 * Accumulate vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_accum);
Datum
vector_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *statearray = PG_GETARG_ARRAYTYPE_P(0);
	Vector	   *newval = PG_GETARG_VECTOR_P(1);
	float8	   *statevalues;
	int			dim;
	bool		newarr;
	float8		n;
	Datum	   *statedatums;
	float	   *x = newval->x;
	ArrayType  *result;

	/* Check array before using */
	statevalues = CheckStateArray(statearray, "vector_accum");
	dim = STATE_DIMS(statearray);
	newarr = dim == 0;

	if (newarr)
		dim = newval->dim;
	else
		CheckExpectedDim(dim, newval->dim);

	n = statevalues[0] + 1.0;

	statedatums = CreateStateDatums(dim);
	statedatums[0] = Float8GetDatum(n);

	if (newarr)
	{
		for (int i = 0; i < dim; i++)
			statedatums[i + 1] = Float8GetDatum((double) x[i]);
	}
	else
	{
		for (int i = 0; i < dim; i++)
		{
			double		v = statevalues[i + 1] + x[i];

			/* Check for overflow */
			if (isinf(v))
				float_overflow_error();

			statedatums[i + 1] = Float8GetDatum(v);
		}
	}

	/* Use float8 array like float4_accum */
	result = construct_array(statedatums, dim + 1,
							 FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

	pfree(statedatums);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * Combine vectors or half vectors (also used for halfvec_combine)
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_combine);
Datum
vector_combine(PG_FUNCTION_ARGS)
{
	/* Must also update parameters of halfvec_combine if modifying */
	ArrayType  *statearray1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *statearray2 = PG_GETARG_ARRAYTYPE_P(1);
	float8	   *statevalues1;
	float8	   *statevalues2;
	float8		n1;
	float8		n2;
	int			dim;
	int			dim1;
	int			dim2;
	Datum	   *statedatums;
	ArrayType  *result;

	/* Check arrays before using */
	statevalues1 = CheckStateArray(statearray1, "vector_combine");
	statevalues2 = CheckStateArray(statearray2, "vector_combine");

	n1 = statevalues1[0];
	n2 = statevalues2[0];

	dim1 = STATE_DIMS(statearray1);
	dim2 = STATE_DIMS(statearray2);

	if (dim1 == 0 && dim2 == 0)
	{
		dim = 0;
		statedatums = CreateStateDatums(dim);
	}
	else if (dim1 == 0)
	{
		dim = dim2;
		CheckDim(dim);
		statedatums = CreateStateDatums(dim);
		for (int i = 0; i < dim; i++)
			statedatums[i + 1] = Float8GetDatum(statevalues2[i + 1]);
	}
	else if (dim2 == 0)
	{
		dim = dim1;
		CheckDim(dim);
		statedatums = CreateStateDatums(dim);
		for (int i = 0; i < dim; i++)
			statedatums[i + 1] = Float8GetDatum(statevalues1[i + 1]);
	}
	else
	{
		dim = dim1;
		CheckDim(dim);
		CheckExpectedDim(dim, dim2);
		statedatums = CreateStateDatums(dim);
		for (int i = 0; i < dim; i++)
		{
			double		v = statevalues1[i + 1] + statevalues2[i + 1];

			/* Check for overflow */
			if (isinf(v))
				float_overflow_error();

			statedatums[i + 1] = Float8GetDatum(v);
		}
	}

	statedatums[0] = Float8GetDatum(n1 + n2);

	result = construct_array(statedatums, dim + 1,
							 FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

	pfree(statedatums);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * Average vectors
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_avg);
Datum
vector_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *statearray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *statevalues;
	float8		n;
	int			dim;
	Vector	   *result;

	/* Check array before using */
	statevalues = CheckStateArray(statearray, "vector_avg");
	n = statevalues[0];

	/* SQL defines AVG of no values to be NULL */
	if (n == 0.0)
		PG_RETURN_NULL();

	/* Create vector */
	dim = STATE_DIMS(statearray);
	CheckDim(dim);
	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
	{
		result->x[i] = (float) (statevalues[i + 1] / n);
		CheckElement(result->x[i]);
	}

	PG_RETURN_POINTER(result);
}

/*
 * Convert sparse vector to dense vector
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(sparsevec_to_vector);
Datum
sparsevec_to_vector(PG_FUNCTION_ARGS)
{
	SparseVector *svec = PG_GETARG_SPARSEVEC_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Vector	   *result;
	int			dim = svec->dim;
	float	   *values = SPARSEVEC_VALUES(svec);

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	result = InitVector(dim);
	for (int i = 0; i < svec->nnz; i++)
	{
		int32		index = svec->indices[i];

		/* Safety check */
		if (index < 0 || index >= dim)
			elog(ERROR, "index out of bounds");

		result->x[index] = values[i];
	}

	PG_RETURN_POINTER(result);
}

#ifndef VECTOR_H
#define VECTOR_H

#include "fmgr.h"
#include "utils/palloc.h"

#if PG_VERSION_NUM < 190000
#include "storage/shmem.h"		/* for add_size()/mul_size() in some versions */
#endif

#define VECTOR_MAX_DIM 16000

#define VECTOR_SIZE(_dim)		add_size(offsetof(Vector, x), mul_size(sizeof(float), (Size) (_dim)))
#define DatumGetVector(x)		((Vector *) PG_DETOAST_DATUM(x))
#define PG_GETARG_VECTOR_P(x)	DatumGetVector(PG_GETARG_DATUM(x))
#define PG_RETURN_VECTOR_P(x)	PG_RETURN_POINTER(x)

typedef struct Vector
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int16		dim;			/* number of dimensions */
	int16		unused;			/* reserved for future use, always zero */
	float		x[FLEXIBLE_ARRAY_MEMBER];
}			Vector;

Vector	   *InitVector(int dim);
void		PrintVector(char *msg, Vector * vector);
int			vector_cmp_internal(Vector * a, Vector * b);

typedef void (*VectorBatchDistFunc) (int dim, const float *ax, const float * const *bx, double *distances, int count);
typedef void (*VectorSoABatchDistFunc) (int dim, const float *q, const float *soa_bx, double *distances, int count);
typedef void (*VectorSoABatchDistFunc_InPlace) (int dim, const float *q, const float *soa_values, double *distances, int count);

VectorBatchDistFunc VectorGetBatchDistFunc(PGFunction fn);
VectorSoABatchDistFunc VectorGetSoABatchDistFunc(PGFunction fn);
VectorSoABatchDistFunc_InPlace VectorGetSoABatchDistFunc_InPlace(PGFunction fn);

void VectorBatchL2SquaredDistance(int dim, const float *ax, const float * const *bx, double *distances, int count);
void VectorBatchL2Distance(int dim, const float *ax, const float * const *bx, double *distances, int count);
void VectorBatchNegativeInnerProduct(int dim, const float *ax, const float * const *bx, double *distances, int count);
void VectorBatchInnerProduct(int dim, const float *ax, const float * const *bx, double *distances, int count);

void VectorBatchL2SquaredDistance_SoA(int dim, const float *q, const float *soa_bx, double *distances, int count);
void VectorBatchL2Distance_SoA(int dim, const float *q, const float *soa_bx, double *distances, int count);
void VectorBatchNegativeInnerProduct_SoA(int dim, const float *q, const float *soa_bx, double *distances, int count);
void VectorBatchInnerProduct_SoA(int dim, const float *q, const float *soa_bx, double *distances, int count);

void VectorBatchL2SquaredDistance_SoA_InPlace(int dim, const float *q, const float *soa_values, double *distances, int count);
void VectorBatchNegativeInnerProduct_SoA_InPlace(int dim, const float *q, const float *soa_values, double *distances, int count);

void VectorBatchL2SquaredDistance_Packed_InPlace(int dim, const float *q, const float *packed_values, double *distances, int count);
void VectorBatchNegativeInnerProduct_Packed_InPlace(int dim, const float *q, const float *packed_values, double *distances, int count);
void *VectorGetPackedBatchDistFunc_InPlace(PGFunction fn);

void VectorBatchL2SquaredDistance_AoSoA_InPlace(int dim, const float *q, const float *aosoa_values, double *distances, int count);
void VectorBatchNegativeInnerProduct_AoSoA_InPlace(int dim, const float *q, const float *aosoa_values, double *distances, int count);
void *VectorGetAoSoABatchDistFunc_InPlace(PGFunction fn);

/* TODO Move to better place */
#if PG_VERSION_NUM >= 160000
#define FUNCTION_PREFIX
#else
#define FUNCTION_PREFIX PGDLLEXPORT
#endif

#endif

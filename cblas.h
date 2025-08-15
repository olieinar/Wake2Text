#pragma once

// Minimal CBLAS implementation for Windows build
// This is a stub implementation for basic functionality

#ifdef __cplusplus
extern "C" {
#endif

// CBLAS types and enums
typedef enum {
    CblasRowMajor = 101,
    CblasColMajor = 102
} CBLAS_ORDER;

typedef enum {
    CblasNoTrans = 111,
    CblasTrans = 112,
    CblasConjTrans = 113
} CBLAS_TRANSPOSE;

// Basic CBLAS functions - stub implementations
static void cblas_sgemm(const CBLAS_ORDER Order,
                       const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB,
                       const int M, const int N, const int K,
                       const float alpha, const float *A, const int lda,
                       const float *B, const int ldb,
                       const float beta, float *C, const int ldc) {
    // Simple row-major matrix multiplication implementation
    // This is not optimized but should work for basic functionality
    if (Order != CblasRowMajor) return; // Only support row major for now
    
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int k = 0; k < K; k++) {
                float a_val = (TransA == CblasNoTrans) ? A[i * lda + k] : A[k * lda + i];
                float b_val = (TransB == CblasNoTrans) ? B[k * ldb + j] : B[j * ldb + k];
                sum += a_val * b_val;
            }
            C[i * ldc + j] = alpha * sum + beta * C[i * ldc + j];
        }
    }
}

static void cblas_sger(const CBLAS_ORDER order,
                      const int M, const int N,
                      const float alpha, const float *X, const int incX,
                      const float *Y, const int incY, float *A, const int lda) {
    // Perform the rank-1 update: A := alpha*x*y' + A
    if (order != CblasRowMajor) return;
    
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            A[i * lda + j] += alpha * X[i * incX] * Y[j * incY];
        }
    }
}

static void cblas_sscal(const int N, const float alpha, float *X, const int incX) {
    // Scale vector X by alpha
    for (int i = 0; i < N; i++) {
        X[i * incX] *= alpha;
    }
}

static float cblas_sdot(const int N, const float *X, const int incX,
                       const float *Y, const int incY) {
    // Compute dot product of two vectors
    float result = 0.0f;
    for (int i = 0; i < N; i++) {
        result += X[i * incX] * Y[i * incY];
    }
    return result;
}

static void cblas_sgemv(const CBLAS_ORDER order,
                       const CBLAS_TRANSPOSE TransA, const int M, const int N,
                       const float alpha, const float *A, const int lda,
                       const float *X, const int incX,
                       const float beta, float *Y, const int incY) {
    // Matrix-vector multiplication: Y := alpha*A*X + beta*Y
    if (order != CblasRowMajor) return;
    
    int rows = (TransA == CblasNoTrans) ? M : N;
    int cols = (TransA == CblasNoTrans) ? N : M;
    
    for (int i = 0; i < rows; i++) {
        float sum = 0.0f;
        for (int j = 0; j < cols; j++) {
            float a_val = (TransA == CblasNoTrans) ? A[i * lda + j] : A[j * lda + i];
            sum += a_val * X[j * incX];
        }
        Y[i * incY] = alpha * sum + beta * Y[i * incY];
    }
}

static void cblas_saxpy(const int N, const float alpha, const float *X, const int incX,
                       float *Y, const int incY) {
    // Y := alpha*X + Y
    for (int i = 0; i < N; i++) {
        Y[i * incY] += alpha * X[i * incX];
    }
}

#ifdef __cplusplus
}
#endif

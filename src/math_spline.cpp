#include "math_spline.h"
#include "math_core.h"
#include "math_fit.h"
#include "utils.h"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace math {

// ------- Some machinery for B-splines ------- //
namespace {

/// linear interpolation on a grid with a special treatment for indices outside the grid
inline double linInt(const double x, const double grid[], int size, int i1, int i2)
{
    double x1 = grid[i1<0 ? 0 : i1>=size ? size-1 : i1];
    double x2 = grid[i2<0 ? 0 : i2>=size ? size-1 : i2];
    if(x1==x2) {
        return x==x1 ? 1 : 0;
    } else {
        return (x-x1) / (x2-x1);
    }
}

/** Compute the values of B-spline functions used for 1d interpolation.
    For any point inside the grid, at most N+1 basis functions are non-zero out of the entire set
    of (N_grid+N-1) basis functions; this routine reports only the nontrivial ones.
    \tparam N   is the degree of spline basis functions;
    \param[in]  x  is the input position on the grid;
    \param[in]  grid  is the array of grid nodes;
    \param[in]  size  is the length of this array;
    \param[out] B  are the values of N+1 possibly nonzero basis functions at this point,
    if the point is outside the grid then all values are zeros;
    \return  the index of the leftmost out of N+1 nontrivial basis functions.
*/
template<int N>
inline int bsplineValues(const double x, const double grid[], int size, double B[])
{
    const int ind = binSearch(x, grid, size);
    if(ind<0 || ind>=size-1) {
        std::fill(B, B+N+1, 0.);
        return ind<0 ? 0 : size-2;
    }

    // de Boor's algorithm:
    // 0th degree basis functions are all zero except the one on the grid segment `ind`
    for(int i=0; i<=N; i++)
        B[i] = i==N ? 1 : 0;
    for(int l=1; l<=N; l++) {
        double Bip1=0;
        for(int i=N, j=ind; j>=ind-l; i--, j--) {
            double Bi = B[i] * linInt(x, grid, size, j, j+l)
                      + Bip1 * linInt(x, grid, size, j+l+1, j+1);
            Bip1 = B[i];
            B[i] = Bi;
        }
    }
    return ind;
}

/// subexpression in b-spline derivatives (inverse distance between grid nodes or 0 if they coincide)
inline double denom(const double grid[], int size, int i1, int i2)
{
    double x1 = grid[i1<0 ? 0 : i1>=size ? size-1 : i1];
    double x2 = grid[i2<0 ? 0 : i2>=size ? size-1 : i2];
    return x1==x2 ? 0 : 1 / (x2-x1);
}

/// recursive template definition for B-spline derivatives through B-spline derivatives
/// of lower degree and order;
/// the arguments are the same as for `bsplineValues`, and `order` is the order of derivative.
template<int N, int Order>
inline int bsplineDerivs(const double x, const double grid[], int size, double B[])
{
    int ind = bsplineDerivs<N-1, Order-1>(x, grid, size, B+1);
    B[0] = 0;
    for(int i=0, j=ind-N; i<=N; i++, j++) {
        B[i] = N * (B[i]   * denom(grid, size, j, j+N)
            + (i<N? B[i+1] * denom(grid, size, j+N+1, j+1) : 0) );
    }
    return ind;
}

/// the above recursion terminates when the order of derivative is zero, returning B-spline values;
/// however, C++ rules do not permit to declare a partial function template specialization
/// (for arbitrary N and order 0), therefore we use full specializations for several values of N
template<>
inline int bsplineDerivs<0,0>(const double x, const double grid[], int size, double B[]) {
    return bsplineValues<0>(x, grid, size, B);
}
template<>
inline int bsplineDerivs<1,0>(const double x, const double grid[], int size, double B[]) {
    return bsplineValues<1>(x, grid, size, B);
}
template<>
inline int bsplineDerivs<2,0>(const double x, const double grid[], int size, double B[]) {
    return bsplineValues<2>(x, grid, size, B);
}
template<>
inline int bsplineDerivs<3,0>(const double x, const double grid[], int size, double B[]) {
    return bsplineValues<3>(x, grid, size, B);
}
template<>
inline int bsplineDerivs<0,1>(const double, const double[], int, double[]) {
    assert(!"Should not be called");
    return 0;
}
template<>
inline int bsplineDerivs<0,2>(const double, const double[], int, double[]) {
    assert(!"Should not be called");
    return 0;
}
template<>
inline int bsplineDerivs<0,3>(const double, const double[], int, double[]) {
    assert(!"Should not be called");
    return 0;
}

/** Similar to bsplineValues, but uses linear extrapolation outside the grid domain */
template<int N>
inline int bsplineValuesExtrapolated(const double x, const double grid[], int size, double B[])
{
    double x0 = fmax(grid[0], fmin(grid[size-1], x));
    int ind = bsplineValues<N>(x0, grid, size, B);
    if(x != x0) {   // extrapolate using the derivatives
        double D[N+1];
        bsplineDerivs<N,1>(x0, grid, size, D);
        for(int i=0; i<=N; i++)
            B[i] += D[i] * (x-x0);
    }
    return ind;
}

/** Compute the matrix of overlap integrals for the array of 1d B-spline functions or their derivs.
    Let N>=1 be the degree of B-splines, and D - the order of derivative in question.
    There are numBasisFnc = numKnots+N-1 basis functions B_p(x) on the entire interval spanned by knots,
    and each of them is nonzero on at most N+1 consecutive sub-intervals between knots.
    Define the matrix M_{pq}, 0<=p<=q<numBasisFnc, to be the symmetric matrix of overlap integrals:
    \f$  M_{pq} = \int dx B^(D)_p(x) B^(D)_q(x)  \f$, where the integrand is nonzero on at most q-p+N+1
    consecutive sub-intervals, and B^(D) is the D'th derivative of the corresponding function.
*/
template<int N, int D>
Matrix<double> computeOverlapMatrix(const std::vector<double> &knots)
{
    int numKnots = knots.size(), numBasisFnc = numKnots+N-1;
    // B-spline of degree N is a polynomial of degree N, so its D'th derivative is a polynomial
    // of degree N-D. To compute the integral of a product of two such functions over a sub-interval,
    // it is sufficient to employ a Gauss-Legendre quadrature rule with the number of nodes = N-D+1.
    const int Nnodes = std::max<int>(N-D+1, 0);
    double glnodes[Nnodes], glweights[Nnodes];
    prepareIntegrationTableGL(0, 1, Nnodes, glnodes, glweights);

    // Collect the values of all possibly non-zero basis functions (or their D'th derivatives)
    // at Nnodes points of each sub-interval between knots. There are at most N+1 such non-zero functions,
    // so these values are stored in a 2d array [N+1] x [number of subintervals * number of GL nodes].
    Matrix<double> values(N+1, (numKnots-1)*Nnodes);
    for(int k=0; k<numKnots-1; k++) {
        double der[N+1];
        for(int n=0; n<Nnodes; n++) {
            // evaluate the possibly non-zero functions and keep track of the index of the leftmost one
            int ind = bsplineDerivs<N, D> ( knots[k] + (knots[k+1] - knots[k]) * glnodes[n],
                &knots.front(), numKnots, der);
            for(int b=0; b<=N; b++)
                values(b, k*Nnodes+n) = der[b+k-ind];
        }
    }

    // evaluate overlap integrals and store them in the symmetric matrix M_pq, which is a banded matrix
    // with nonzero values only within N+1 cells from the diagonal
    Matrix<double> mat(numBasisFnc, numBasisFnc, 0);
    for(int p=0; p<numBasisFnc; p++) {
        int kmin = std::max<int>(p-N, 0);   // index of leftmost knot of the integration sub-intervals
        int kmax = std::min<int>(p+1, numKnots-1);     // same for the rightmost
        int qmax = std::min<int>(p+N+1, numBasisFnc);  // max index of the column of the banded matrix
        for(int q=p; q<qmax; q++) {
            double result = 0;
            // loop over sub-intervals where the integrand might be nonzero
            for(int k=kmin; k<kmax; k++) {
                double dx = knots[k+1]-knots[k];
                // loop over nodes of GL quadrature rule over the sub-interval
                for(int n=0; n<Nnodes; n++) {
                    double P = p>=k && p<=k+N ? values(p-k, k*Nnodes+n) : 0;
                    double Q = q>=k && q<=k+N ? values(q-k, k*Nnodes+n) : 0;
                    result  += P * Q * glweights[n] * dx;
                }
            }
            mat(p, q) = result;
            mat(q, p) = result;  // it is symmetric
        }
    }
    return mat;
}

/** Sparse matrix with a special structure:
    Nrows >> Ncols, each i'th row contains exactly Nvals entries
    in contiguous columns col[ indcol[i] ] .. col[ indcol[i] + Nvals - 1 ].
    This type of matrix arises in penalized spline fitting and density estimation contexts,
    where Nrows is the number of data points, the Ncols is the number of basis functions,
    and only Nvals of them are nonzero for each data point.
*/
template<int Nvals>
class SparseMatrixSpecial {
    unsigned int nRows;               ///< number of rows (large)
    unsigned int nCols;               ///< number of columns (small)
    std::vector<double> values;       ///< all entries stored in a single array Nrow * Nval
    std::vector<unsigned int> indcol; ///< indices of the first column in each row
public:
    SparseMatrixSpecial(unsigned int Nrows, unsigned int Ncols) :
        nRows(Nrows), nCols(Ncols), values(Nrows * Nvals, 0), indcol(Nrows, 0) {}

    /// assign the entire row of the matrix (may be used in parallel loops)
    inline void assignRow(unsigned int row, unsigned int firstColumnIndex, const double vals[])
    {
        assert(firstColumnIndex + Nvals <= nCols && row<nRows);
        indcol[row] = firstColumnIndex;
        for(int c=0; c<Nvals; c++)
            values[row * Nvals + c] = vals[c];
    }

    /** compute M^T diag(weights) M  -- the product of the matrix M with its transpose,
        with an optional diagonal weight matrix in between;
        a banded symmetric square matrix of size Ncols*Ncols with bandwidth 2*Nvals-1.
        The weights may be an empty vector (meaning they are unity), or a vector of length Nrows. */
    Matrix<double> multiplyByTransposed(
        const std::vector<double>& weights = std::vector<double>()) const
    {
        bool noWeights = weights.empty();
        assert(values.size() == nRows * Nvals && indcol.size() == nRows &&
            (noWeights || weights.size() == nRows));
        Matrix<double> result(nCols, nCols, 0.);
        for(unsigned int row=0; row<nRows; row++) {
            unsigned int ind = indcol[row];
            double weight = noWeights ? 1. : weights[row];
            for(int i=0; i<Nvals; i++)
                for(int j=i; j<Nvals; j++)  // fill in only the upper triangle of the symmetric matrix
                    result(ind+i, ind+j) += weight * values[row*Nvals+i] * values[row*Nvals+j];
        }
        for(unsigned int i=1; i<nCols; i++) // fill the remaining lower triangle
            for(unsigned int j=0; j<i; j++)
                result(i, j) = result(j, i);
        return result;
    }

    /** compute M^T diag(weights) vec,
        where `vec` and `weights` are vectors of length Nrow
        (the latter may be empty, meaning that weights are unity);
        the result is a vector of length Ncol. */
    std::vector<double> multiplyByVector(
        const std::vector<double>& vec,
        const std::vector<double>& weights = std::vector<double>()) const
    {
        bool noWeights = weights.empty();
        assert(vec.size() == nRows && (noWeights || weights.size() == nRows));
        std::vector<double> result(nCols, 0.);
        for(unsigned int row=0; row<nRows; row++) {
            unsigned int ind = indcol[row];
            double val = noWeights ? vec[row] : weights[row] * vec[row];
            for(int i=0; i<Nvals; i++)
                result[ind+i] += val * values[row*Nvals+i];
        }
        return result;
    }
};

/// definite integral of x^(m+n)
class MonomialIntegral: public IFunctionIntegral {
    const int n;
public:
    MonomialIntegral(int _n) : n(_n) {};
    virtual double integrate(double x1, double x2, int m=0) const {
        return m+n+1==0 ? log(x2/x1) : (pow(x2, m+n+1) - pow(x1, m+n+1)) / (m+n+1);
    }
};


//---- spline construction routines ----//

/// compute the first derivatives of a natural or clamped cubic spline from the condition
/// that the second derivatives are continuous at all interior grid nodes
std::vector<double> constructCubicSpline(const std::vector<double>& xval,
    const std::vector<double>& fval, double derivLeft=NAN, double derivRight=NAN)
{
    size_t numPoints = xval.size();
    if(fval.size() != numPoints)        
        throw std::length_error("CubicSpline: input arrays are not equal in length");

    // construct and solve the linear system for first derivatives
    std::vector<double> rhs(numPoints);
    BandMatrix<double>  mat(numPoints, 1, 0.);
    for(size_t i = 0; i < numPoints-1; i++) {
        const double
        dxi = 1 / (xval[i+1] - xval[i]),
        dyx = (fval[i+1] - fval[i]) * pow_2(dxi);
        mat(i,  i)   += 2 * dxi;  // diag current row
        mat(i+1,i+1) += 2 * dxi;  // diag next row
        mat(i,  i+1)  = dxi;      // above-diag current
        mat(i+1,i)    = dxi;      // below-diag next
        rhs[i]       += 3 * dyx;
        rhs[i+1]     += 3 * dyx;
    }
    if(isFinite(derivLeft)) {   // for a clamped spline, the derivative at the endpoint is already known
        mat(0,0) = 1;           // diag
        mat(0,1) = 0;           // above
        rhs[0]   = derivLeft;
    }
    if(isFinite(derivRight)) {
        mat(numPoints-1,numPoints-1) = 1; // diag
        mat(numPoints-1,numPoints-2) = 0; // below
        rhs[numPoints-1] = derivRight;
    }
    return solveBand(mat, rhs);
}

// compute the 2nd derivative at each grid node from the condition that the 3rd derivative
// is continuous at all interior nodes, and zero at the boundary nodes.
std::vector<double> constructQuinticSpline(const std::vector<double>& xval,
    const std::vector<double>& fval, const std::vector<double>& fder)
{
    size_t numPoints = xval.size();
    BandMatrix<double>  mat(numPoints, 1, 0.);
    std::vector<double> rhs(numPoints);
    for(size_t i = 0; i < numPoints; i++) {
        if(i > 0) {
            double hi = 1 / (xval[i] - xval[i-1]);
            mat(i,i) += hi * 3;  // diagonal element
            mat(i-1,i)= -hi;     // above-diagonal
            rhs[i]   -= (20 * (fval[i] - fval[i-1]) * hi - 12 * fder[i] - 8 * fder[i-1]) * hi * hi;
        }
        if(i < numPoints-1) {
            double hi = 1 / (xval[i+1] - xval[i]);
            mat(i,i) += hi * 3;
            mat(i+1,i)= -hi;     // below-diagonal
            rhs[i]   += (20 * (fval[i+1] - fval[i]) * hi - 12 * fder[i] - 8 * fder[i+1]) * hi * hi;
        }
    }
#if 1
    // alternative boundary conditions: 4th derivative is zero at boundary nodes --
    // seems to give a better overall accuracy, and is different from the original Dehnen's pspline
    double hi = 1 / (xval[1] - xval[0]);
    mat(0,1)  = -2*hi;  // above
    rhs[0]    = (30 * (fval[1] - fval[0]) * hi - 14 * fder[1] - 16 * fder[0]) * hi * hi;
    hi        = 1 / (xval[numPoints-1] - xval[numPoints-2]);
    mat(numPoints-1,numPoints-2) = -2*hi;  // below
    rhs[numPoints-1]   = ( -30 * (fval[numPoints-1] - fval[numPoints-2]) * hi
        + 14 * fder[numPoints-2] + 16 * fder[numPoints-1]) * hi * hi;
#endif
    return solveBand(mat, rhs);
}

//---- spline evaluation routines ----//

/// compute the value, derivative and 2nd derivative of (possibly several, K>=1) cubic spline(s);
/// input arguments contain the value(s) and 1st derivative(s) of these splines
/// at the boundaries of interval [xl..xh] that contain the point x.
template<unsigned int K>
inline void evalCubicSplines(
    const double x,    // input:   value of x at which the spline is computed (xl <= x <= xh)
    const double xl,   // input:   lower boundary of the interval
    const double xh,   // input:   upper boundary of the interval
    const double* fl,  // input:   f_k (xl)
    const double* fh,  // input:   f_k (xh)
    const double* dl,  // input:   df_k/dx (xl)
    const double* dh,  // input:   df_k/dx (xh)
    double* f,         // output:  f_k(x)      if f   != NULL
    double* df,        // output:  df_k/dx     if df  != NULL
    double* d2f)       // output:  d^2f_k/dx^2 if d2f != NULL
{
    const double
        h      =  xh - xl,
        hi     =  1 / h,
        t      =  (x-xl) / h,  // NOT (x-xl)*hi, because this doesn't always give an exact result if x==xh
        T      =  1-t,
#if 0   // two alternative ways of computing - not clear which one is more efficient
        tq     =  t*t,
        Tq     =  T*T,
        f_fl   =  Tq * (1+2*t),
        f_fh   =  tq * (1+2*T),
        f_dl   =  Tq * (x-xl),
        f_dh   = -tq * (xh-x),
        df_dl  =  T  * (1-3*t),
        df_dh  =  t  * (1-3*T),
        df_dif =  6  * t*T * hi,
        d2f_dl =  (6*t-4) * hi,
        d2f_dh = -(6*T-4) * hi,
        d2f_dif= -(d2f_dl + d2f_dh) * hi;
    for(unsigned int k=0; k<K; k++) {
        const double dif = fh[k] - fl[k];
        if(f)
            f[k]   = dl[k] *   f_dl  +  dh[k] *   f_dh  +  fl[k] * f_fl  +  fh[k] * f_fh;
        if(df)
            df[k]  = dl[k] *  df_dl  +  dh[k] *  df_dh  +  dif *  df_dif;
        if(d2f)
            d2f[k] = dl[k] * d2f_dl  +  dh[k] * d2f_dh  +  dif * d2f_dif;
    }
#else
    tt = t*t,
    tT = t*T;
    for(unsigned int k=0; k<K; k++) {
        const double Q = dl[k] + dh[k] - 2 * (fh[k] - fl[k]) * hi;
        if(f)
            f[k]   = fl[k] * (1-tt)  +  fh[k] * tt  +  (dl[k] - t * Q) * tT * h;
        if(df)
            df[k]  = dl[k] * T       +  dh[k] * t   -  3*Q * tT;
        if(d2f)
            d2f[k] = (dh[k] - dl[k]  +  3*Q * (t-T)) * hi;
    }
#endif
}

/// compute the value, derivative and 2nd derivative of (possibly several, K>=1) quintic spline(s);
/// input arguments contain the value(s), 1st and 2rd derivative(s) of these splines
/// at the boundaries of interval [xl..xh] that contain the point x.
template<unsigned int K>
inline void evalQuinticSplines(
    const double x,    // input:   value of x at which the spline is computed (xl <= x <= xh)
    const double xl,   // input:   lower boundary of the interval
    const double xh,   // input:   upper boundary of the interval
    const double* fl,  // input:   f_k(xl), k=0..K-1
    const double* fh,  // input:   f_k(xh)
    const double* f1l, // input:   df_k(xl)
    const double* f1h, // input:   df_k(xh)
    const double* f2l, // input:   d2f_k(xl)
    const double* f2h, // input:   d2f_k(xh)
    double* f,         // output:  f_k(x)      if f   != NULL
    double* df,        // output:  df_k/dx     if df  != NULL
    double* d2f)       // output:  d^2f_k/dx^2 if d2f != NULL
{
    if(x==xh) {  // special treatment of x exactly at the rightmost boundary, to avoid rounding errors
        for(unsigned int k=0; k<K; k++) {
            if(f)
                f[k]   = fh[k];
            if(df)
                df[k]  = f1h[k];
            if(d2f)
                d2f[k] = f2h[k];
        }
        return;
    }
    const double
    dx  = x  - xl,
    dx2 = .5 * dx * dx,
    h   = xh - xl,
    hi  = 1  / h,
    h2  = h  * h,
    t   = dx * hi,
    t2  = t  * t,
    t3  = t  * t2,
    t1t = t  * (1-t),
    P   = t3 * (10- t * (15 - 6*t)),
    Q   = t3 * (1 - t * 0.5) * h,
    R   = t3 * (1 - t * (1.25 - 0.5*t)) * h2,
    Px  = 30 * t1t * hi,
    Pp  = Px * t1t,
    Qp  = t2 * (3 - t * 2),
    Rp  = t2 * (3 - t * (5 - 2.5*t)) * h,
    Ppp = Px * hi * (2 - 4*t),
    Qpp = Px * 0.2,
    Rpp = t  * (6 - t * (15 - 10*t));
    for(unsigned int k=0; k<K; k++) {
        double
        fd  = fh [k] - fl [k] - 0.5*h * (f1h[k] + f1l[k]),
        f1d = f1h[k] - f1l[k] - 0.5*h * (f2h[k] + f2l[k]),
        f2d = f2h[k] - f2l[k];
        if(f)
            f[k]   = fl[k] + P * fd + dx * f1l[k] +   Q * f1d + dx2 * f2l[k] +   R * f2d;
        if(df)
            df[k]  =        Pp * fd +      f1l[k] +  Qp * f1d +  dx * f2l[k] +  Rp * f2d;
        if(d2f)
            d2f[k] =       Ppp * fd +               Qpp * f1d +       f2l[k] + Rpp * f2d;
    }
}
    
//---- Auxiliary spline construction routines ----//

/// apply the slope-limiting prescription of Hyman(1983) to the first derivatives of a previously
/// constructed natural cubic spline (modify the first derivatives if necessary)
inline void regularizeSpline(const std::vector<double>& xval,
    const std::vector<double>& fval, std::vector<double>& fder)
{
    // slightly reduce the maximum slope to make the interpolator strictly monotonic when possible
    const double THREE = 3 - 3e-15;
    size_t numPoints = xval.size();
    assert(fval.size() == numPoints && fder.size() == numPoints);
    for(size_t i=0; i<numPoints; i++) {
        if(i==0 || i==numPoints-1) {  // boundary points
            size_t k = i==0 ? i : numPoints-2;
            double sec = (fval[k+1] - fval[k]) / (xval[k+1] - xval[k]);
            fder[i] = sec>=0 ?
                fmin( fmax(fder[i], 0.), THREE * sec) :
                fmax( fmin(fder[i], 0.), THREE * sec);
        } else {  // interior points
            double secL = (fval[i]   - fval[i-1]) / (xval[i]   - xval[i-1]);
            double secR = (fval[i+1] - fval[i]  ) / (xval[i+1] - xval[i]  );
            fder[i] = fmin( fabs(fder[i]), THREE * fmin( fabs(secL), fabs(secR) ) ) *
                (secL * secR >= 0 ? sign(secL) : sign(fder[i]));
        }
    }
}

}  // internal namespace


BaseInterpolator1d::BaseInterpolator1d(const std::vector<double>& xv, const std::vector<double>& fv) :
    xval(xv), fval(fv)
{
    if(xv.size() < 2)
        throw std::invalid_argument("Error in 1d interpolator: number of nodes should be >=2");
    for(unsigned int i=0; i<xv.size(); i++) {
        if(!isFinite(xv[i]))
            throw std::invalid_argument("Error in 1d interpolator: x coordinates must be finite "
                "(x["+utils::toString(i)+"]="+utils::toString(xv[i])+")\n" + utils::stacktrace());
        if(i>0 && xv[i] <= xv[i-1])
            throw std::invalid_argument("Error in 1d interpolator: "
                "x values must be monotonically increasing "
                "(x["+utils::toString(i) + "]="+utils::toString(xv[i]) + " is not greater than"
                " x["+utils::toString(i-1)+"]="+utils::toString(xv[i-1])+")\n" + utils::stacktrace());
    }
    for(unsigned int i=0; i<fv.size(); i++)
        if(!isFinite(fv[i]))
            throw std::invalid_argument("Error in 1d interpolator: function values must be finite "
                "(f["+utils::toString(i)+"]="+utils::toString(fv[i])+")\n" + utils::stacktrace());
}

LinearInterpolator::LinearInterpolator(const std::vector<double>& xv, const std::vector<double>& yv) :
    BaseInterpolator1d(xv, yv)
{
    if(fval.size() != xval.size())
        throw std::length_error("LinearInterpolator: input arrays are not equal in length");
}

void LinearInterpolator::evalDeriv(const double x, double* value, double* deriv, double* deriv2) const
{
    int i = std::max<int>(0, std::min<int>(xval.size()-2, binSearch(x, &xval[0], xval.size())));
    if(value)
        *value = linearInterp(x, xval[i], xval[i+1], fval[i], fval[i+1]);
    if(deriv)
        *deriv = (fval[i+1]-fval[i]) / (xval[i+1]-xval[i]);
    if(deriv2)
        *deriv2 = 0;
}


//-------------- CUBIC SPLINE --------------//

// initialization from function values or from amplitudes of B-spline interpolator
CubicSpline::CubicSpline(const std::vector<double>& xvalues, const std::vector<double>& fvalues) :
    BaseInterpolator1d(xvalues, fvalues)
{
    size_t numPoints = xval.size();
    if(fval.size() == numPoints+2) {
        // initialize from the amplitudes of B-splines defined at these nodes
        std::vector<double> ampl(fvalues);  // temporarily store the amplitudes of B-splines
        fval.assign(numPoints, 0);
        fder.resize(numPoints);
        for(size_t i=0; i<numPoints; i++) {
            // compute values and first derivatives of B-splines at grid nodes
            double val[4], der[4];
            int ind = bsplineValues<3>(xval[i], &xval[0], numPoints, val);
            bsplineDerivs<3,1>(xval[i], &xval[0], numPoints, der);
            for(int p=0; p<=3; p++) {
                fval[i] += val[p] * ampl[p+ind];
                fder[i] += der[p] * ampl[p+ind];
            }
        }
    } else   // create a natural cubic spline
        fder = constructCubicSpline(xval, fval);
}

// initialization from function values and two endpoint derivatives (a clamped spline)
CubicSpline::CubicSpline(const std::vector<double>& xvalues, const std::vector<double>& fvalues,
    double derivLeft, double derivRight) :
    BaseInterpolator1d(xvalues, fvalues)
{
    fder = constructCubicSpline(xval, fval, derivLeft, derivRight);
}

// initialization from function values and optional regularization procedure
CubicSpline::CubicSpline(const std::vector<double>& xvalues, const std::vector<double>& fvalues,
    bool regularize) :
    BaseInterpolator1d(xvalues, fvalues)
{
    fder = constructCubicSpline(xval, fval);
    if(regularize)
        regularizeSpline(xval, fval, fder);
}

// initialization from function values and derivatives at all grid nodes
CubicSpline::CubicSpline(const std::vector<double>& _xval,
    const std::vector<double>& _fval, const std::vector<double>& _fder) :
    BaseInterpolator1d(_xval, _fval), fder(_fder)
{
    if(fval.size() != xval.size() || fder.size() != xval.size())
        throw std::length_error("CubicSpline: input arrays are not equal in length");
}

void CubicSpline::evalDeriv(const double x, double* val, double* deriv, double* deriv2) const
{
    int size = xval.size();
    if(size == 0)
        throw std::length_error("Empty spline");
    int index = binSearch(x, &xval[0], size);
    if(index < 0) {
        if(val)
            *val   = fval[0] + (fder[0]==0 ? 0 : fder[0] * (x-xval[0]));
            // if der==0, will give correct result even for infinite x
        if(deriv)
            *deriv = fder[0];
        if(deriv2)
            *deriv2= 0;
        return;
    }
    if(index >= size) {
        if(val)
            *val   = fval[size-1] + (fder[size-1]==0 ? 0 : fder[size-1] * (x-xval[size-1]));
        if(deriv)
            *deriv = fder[size-1];
        if(deriv2)
            *deriv2= 0;
        return;
    }
    evalCubicSplines<1> (x, xval[index], xval[index+1],
        &fval[index], &fval[index+1], &fder[index], &fder[index+1],
        /*output*/ val, deriv, deriv2);
}

bool CubicSpline::isMonotonic() const
{
    if(fval.empty())
        throw std::length_error("Empty spline");
    bool ismonotonic=true;
    for(unsigned int index=0; ismonotonic && index < xval.size()-1; index++) {
        const double
        dx = xval[index+1] - xval[index],
        dy = fval[index+1] - fval[index],
        dl = fder[index],
        dh = fder[index+1];
        if( dl * dh < 0 )   // derivs have opposite signs at two endpoints - clearly non-monotonic
            ismonotonic = false;
        else {
            // derivative is  a * t^2 + b * t + c,  with 0<=t<=1 on the given interval.
            double a  = 3 * (dl + dh - dy / dx * 2), ba = -1 + (dh - dl) / a,  ca = dl / a,
            D  = ba * ba - 4 * ca;   // discriminant of the above quadratic equation
            if(a != 0 && D > 0) {    // need to check roots
                double x1 = 0.5 * (-ba-sqrt(D));
                double x2 = 0.5 * (-ba+sqrt(D));
                if( (x1>0 && x1<1) || (x2>0 && x2<1) )
                    ismonotonic = false;    // there is a root ( y'=0 ) somewhere on the given interval
            }  // otherwise there are no roots
            // note: this is prone to roundoff errors when the spline is semi-monotonic
            // (i.e. has a zero derivative at some point inside the interval, likely at one of its ends),
            // which commonly happens after applying the regularization filter
        }
    }
    return ismonotonic;
}

double CubicSpline::integrate(double x1, double x2, int n) const {
    return integrate(x1, x2, MonomialIntegral(n));
}

double CubicSpline::integrate(double x1, double x2, const IFunctionIntegral& f) const
{
    if(x1==x2)
        return 0;
    if(x1>x2)
        return integrate(x2, x1, f);
    if(fval.empty())
        throw std::length_error("Empty spline");
    double result = 0;
    unsigned int size = xval.size();
    if(x1 <= xval[0]) {    // spline is linearly extrapolated at x<xval[0]
        double X2 = fmin(x2, xval[0]);
        result +=
            f.integrate(x1, X2, 0) * (fval[0] - fder[0] * xval[0]) +
            f.integrate(x1, X2, 1) * fder[0];
        if(x2<=xval[0])
            return result;
        x1 = xval[0];
    }
    if(x2 >= xval[size-1]) {    // same for x>xval[end]
        double X1 = fmax(x1, xval[size-1]);
        result +=
            f.integrate(X1, x2, 0) * (fval[size-1] - fder[size-1] * xval[size-1]) +
            f.integrate(X1, x2, 1) * fder[size-1];
        if(x1>=xval[size-1])
            return result;
        x2 = xval[size-1];
    }
    unsigned int i1 = binSearch(x1, &xval.front(), size);
    unsigned int i2 = binSearch(x2, &xval.front(), size);
    for(unsigned int i=i1; i<=i2; i++) {
        double x  = xval[i];
        double h  = xval[i+1] - x;
        double a  = fval[i];
        double b  = fder[i];
        double d  = (-12 * (fval[i+1] - a) / h + 6 * (fder[i+1] + b)) / pow_2(h);
        double c  = -0.5 * h * d + (fder[i+1] - b) / h;
        // spline(x) = fval[i] + dx * (b + dx * (c/2 + dx*d/6)), where dx = x-xval[i]
        double X1 = i==i1 ? x1 : x;
        double X2 = i==i2 ? x2 : xval[i+1];
        result   +=
            f.integrate(X1, X2, 0) * (a - x * (b - 1./2 * x * (c - 1./3 * x * d))) +
            f.integrate(X1, X2, 1) * (b - x * (c - 1./2 * x * d)) +
            f.integrate(X1, X2, 2) * (c - x * d) / 2 +
            f.integrate(X1, X2, 3) * d / 6;
    }
    return result;
}

// ------ Quintic spline ------- //

QuinticSpline::QuinticSpline(const std::vector<double>& xvalues,
    const std::vector<double>& fvalues, const std::vector<double>& fderivs):
    BaseInterpolator1d(xvalues, fvalues), fder(fderivs)
{
    unsigned int numPoints = xval.size();
    if(fval.size() != numPoints || fder.size() != numPoints)
        throw std::length_error("QuinticSpline: input arrays are not equal in length");
    for(unsigned int i=0; i<numPoints; i++)
        if(!isFinite(fder[i]))
            throw std::invalid_argument("QuinticSpline: function derivatives must be finite "
                "(fder["+utils::toString(i)+"]="+utils::toString(fder[i])+")\n" + utils::stacktrace());
    fder2 = constructQuinticSpline(xval, fval, fder);
}

void QuinticSpline::evalDeriv(const double x, double* val, double* deriv, double* deriv2) const
{
    int size = xval.size();
    if(size == 0)
        throw std::length_error("Empty spline");
    int index = binSearch(x, &xval[0], size);
    if(index < 0) {
        if(val)
            *val   = fval[0] + (fder[0]==0 ? 0 : fder[0] * (x-xval[0]));
        if(deriv)
            *deriv = fder[0];
        if(deriv2)
            *deriv2= 0;
        return;
    }
    if(index >= size) {
        if(val)
            *val   = fval[size-1] + (fder[size-1]==0 ? 0 : fder[size-1] * (x-xval[size-1]));
        if(deriv)
            *deriv = fder[size-1];
        if(deriv2)
            *deriv2= 0;
        return;
    }
    evalQuinticSplines<1> (x, xval[index], xval[index+1],
        &fval[index], &fval[index+1], &fder[index], &fder[index+1], &fder2[index], &fder2[index+1],
        /*output*/ val, deriv, deriv2);
}


// ------ Doubly-log-scaled spline ------ //

LogLogSpline::LogLogSpline(const std::vector<double>& xvalues, const std::vector<double>& fvalues,
    double derivLeft, double derivRight) :
    BaseInterpolator1d(xvalues, fvalues)
{
    size_t numPoints = fvalues.size();
    if(numPoints != xvalues.size())
        throw std::length_error("LogLogSpline: input arrays not equal in length");

    // first initialize the derivatives for an un-scaled spline that will be used as a backup option
    fder = constructCubicSpline(xval, fval, derivLeft, derivRight);
    regularizeSpline(xval, fval, fder);

    logxval.resize(numPoints);
    logfval.resize(numPoints);
    logfder.resize(numPoints, NAN);  // by default points are marked as 'bad'
    std::transform(xvalues.begin(), xvalues.end(), logxval.begin(), log);
    std::transform(fvalues.begin(), fvalues.end(), logfval.begin(), log);

    // construct spline(s) for the sections of x grid where the function values are strictly positive
    std::vector<double> tmpx, tmpf, tmpd;
    for(size_t i=0; i<=numPoints; i++) {
        if(i<numPoints && (!isFinite(logxval[i]) || !isFinite(fvalues[i])))
            throw std::invalid_argument("LogLogSpline: " + utils::toString(i) + "'th element is "
                "f(" + utils::toString(xvalues[i]) + ")=" + utils::toString(fvalues[i]) + "\n" +
                utils::stacktrace());

        if(i==numPoints || !isFinite(logfval[i])) {
            // not so bad point, just can't be represented with a log (or we reached the end):
            // finalize the previous section for log-scaled spline
            size_t numSegPoints = tmpx.size();
            if(numSegPoints > 1) {

                // supply the endpoint derivatives if they were provided and are within this segment
                double derLeft = NAN, derRight = NAN;
                if(i == numSegPoints && isFinite(derivLeft))
                    derLeft = derivLeft  * xvalues.front() / fvalues.front();
                if(i == numPoints && isFinite(derivRight))
                    derRight = derivRight * xvalues.back() / fvalues.back();

                // initialize (log-log) derivs using the spline construction routine
                tmpd = constructCubicSpline(tmpx, tmpf, derLeft, derRight);

                // limit the derivatives so that monotonic segments will be monotonically interpolated
                // (breaking this rule hits particularly badly if the interpolated value will be exp'ed)
                regularizeSpline(tmpx, tmpf, tmpd);

                // assign the log-log derivs for all points in the preceding section
                for(size_t k=0; k<numSegPoints; k++)
                    logfder[i+k-numSegPoints] = tmpd[k];
            }
            tmpx.clear();
            tmpf.clear();
        } else {
            // good point: append it to the preceding section
            tmpx.push_back(logxval[i]);
            tmpf.push_back(logfval[i]);
        }
    }
}

LogLogSpline::LogLogSpline(const std::vector<double>& xvalues, const std::vector<double>& fvalues,
    const std::vector<double>& fderivs) :
    BaseInterpolator1d(xvalues, fvalues),
    fder(fderivs)   // store the original non-scaled derivatives
{
    size_t numPoints = fvalues.size();
    if(numPoints != xvalues.size() || numPoints != fderivs.size())
        throw std::length_error("LogLogSpline: input arrays not equal in length");
    logxval.resize(numPoints);
    logfval. resize(numPoints);
    logfder. resize(numPoints, NAN);
    logfder2.resize(numPoints);
    std::transform(xvalues.begin(), xvalues.end(), logxval.begin(), log);
    std::transform(fvalues.begin(), fvalues.end(), logfval.begin(), log);
    
    // construct spline(s) for the sections of x grid where the function values are strictly positive
    std::vector<double> tmpx, tmpf, tmpd, tmpd2;
    for(size_t i=0; i<=numPoints; i++) {
        bool excluded = false;
        if(i==numPoints)   // we use one extra point to finalize the previous section automatically
            excluded = true;
        else {
            if(!isFinite(logxval[i]) || !isFinite(fvalues[i]) || !isFinite(fderivs[i]))
                throw std::invalid_argument("LogLogSpline: " + utils::toString(i) + "'th element is "
                    "f(" + utils::toString(xvalues[i]) + ")=" + utils::toString(fvalues[i]) +
                    ", f'=" + utils::toString(fderivs[i]) + "\n" + utils::stacktrace());
            // obviously exclude points with non-positive function values
            if(!isFinite(logfval[i]))
                excluded = true;
            // also exclude points for which the cubic interpolation in log-scaled coords
            // would lead to a non-monotonic result
            double logSlopeLeft  = i>0 ?
                (logfval[i] - logfval[i-1]) / (logxval[i] - logxval[i-1]) : INFINITY;
            double logSlopeRight = i<numPoints-1 ?
                (logfval[i+1] - logfval[i]) / (logxval[i+1] - logxval[i]) : INFINITY;
            double logDer = xvalues[i] * fderivs[i] / fvalues[i];
            // for endpoints, check that the sign of derivative is the same as the secant
            if(i==0 && logDer * logSlopeRight < 0)
                excluded = true;
            if(i==numPoints-1 && logDer * logSlopeLeft < 0)
                excluded = true;
            // for all points check that the derivative is no larger than
            // three times the minimum of the slopes of adjacent segments
            if(fabs(logDer) > 3 * std::min(fabs(logSlopeLeft), fabs(logSlopeRight)))
                excluded = true;
        }
        if(excluded) {
            // finalize the previous section for log-scaled spline
            size_t numSegPoints = tmpx.size();
            if(numSegPoints > 1) {

                // initialize (log-log) 2nd derivs using the spline construction routine
                tmpd2 = constructQuinticSpline(tmpx, tmpf, tmpd);
                
                // assign the log-log 1st/2nd derivs for all points in the preceding section
                for(size_t k=0; k<numSegPoints; k++) {
                    logfder [i+k-numSegPoints] = tmpd [k];
                    logfder2[i+k-numSegPoints] = tmpd2[k];
                }
            }
            tmpx.clear();
            tmpf.clear();
            tmpd.clear();
        } else {
            // good point: append it to the preceding section
            tmpx.push_back(logxval[i]);
            tmpf.push_back(logfval[i]);
            tmpd.push_back(xvalues[i] * fderivs[i] / fvalues[i]);
        }
    }
}

void LogLogSpline::evalDeriv(const double x, double* value, double* deriv, double* deriv2) const
{
    int size = xval.size();
    if(size == 0)
        throw std::length_error("Empty spline");
    int index = binSearch(x, &xval[0], size);
    double logx = log(x);

    if(index < 0 || index >= size) {
        index = (index<0 ? 0 : size-1);
        if(!isFinite(logfder[index]) || logfder[index]==0) {
            // either the endpoint was excluded from the log-spline construction,
            // or the endpoint derivative is zero: extrapolate as a constant
            if(value)  *value = fval[index];
            if(deriv)  *deriv = 0.;
            if(deriv2) *deriv2= 0.;
            return;
        }
        // otherwise extrapolate the log-log scaled function linearly,
        // i.e. the original function as a power law with a slope equal to the log-log-derivative
        double fncvalue = fval[index] * exp((logx - logxval[index]) * logfder[index]);
        if(value)
            *value = fncvalue;
        if(deriv)
            *deriv = fncvalue * logfder[index] / x;
        if(deriv2)
            *deriv2= fncvalue * logfder[index] * (logfder[index]-1) / pow_2(x);
        return;
    }

    // otherwise determine whether we are in a valid log-log-interpolated segment,
    // i.e. both endpoints had positive function value (indicated by logfder != NAN)
    if(isFinite(logfder[index] + logfder[index+1])) {
        // yes: perform a cubic or quintic interpolation
        double logfvalue, logfderiv, logfderiv2;
        if(logfder2.empty())
            evalCubicSplines<1>   (logx, logxval[index], logxval[index+1],
                &logfval[index],  &logfval[index+1],  &logfder[index],  &logfder[index+1],
                /*output*/ &logfvalue, &logfderiv, &logfderiv2);
        else
            evalQuinticSplines<1> (logx, logxval[index], logxval[index+1],
                &logfval[index],  &logfval[index+1],  &logfder[index],  &logfder[index+1],
                &logfder2[index], &logfder2[index+1],
                /*output*/ &logfvalue, &logfderiv, &logfderiv2);
        double fvalue = exp(logfvalue);
        if(value)
            *value = fvalue;
        if(deriv)
            *deriv = logfderiv * fvalue / x;
        if(deriv2)
            *deriv2 = (logfderiv2 + logfderiv * (logfderiv-1)) * fvalue / pow_2(x);
    } else {
        // this segment was excluded from log-interpolation
        // because the function is non-positive at one or both endpoints.
        // instead will do a cubic interpolation for this grid segment in un-scaled coordinates,
        // using the first derivatives that were either provided as input or spline-constructed
        evalCubicSplines<1>(x, xval[index], xval[index+1],
            &fval[index], &fval[index+1], &fder[index], &fder[index+1],
            /*output*/ value, deriv, deriv2);
    }
}


// ------ B-spline interpolator ------ //

template<int N>
BsplineInterpolator1d<N>::BsplineInterpolator1d(const std::vector<double>& xgrid) :
    xnodes(xgrid), numComp(xnodes.size()+N-1)
{
    if(xnodes.size()<2)
        throw std::invalid_argument("BsplineInterpolator1d: number of nodes is too small");
    bool monotonic = true;
    for(unsigned int i=1; i<xnodes.size(); i++)
        monotonic &= xnodes[i-1] < xnodes[i];
    if(!monotonic)
        throw std::invalid_argument("BsplineInterpolator1d: grid nodes must be sorted in ascending order");
}

template<int N>
unsigned int BsplineInterpolator1d<N>::nonzeroComponents(
    const double x, const unsigned int derivOrder, double values[]) const
{
    if(derivOrder > N) {
        std::fill(values, values+N+1, 0.);
        return std::max(0, std::min<int>(numComp-N-1, binSearch(x, &xnodes[0], xnodes.size())));
    }
    switch(derivOrder) {
        case 0: return bsplineValues<N>  (x, &xnodes[0], xnodes.size(), values);
        case 1: return bsplineDerivs<N,1>(x, &xnodes[0], xnodes.size(), values);
        case 2: return bsplineDerivs<N,2>(x, &xnodes[0], xnodes.size(), values);
        case 3: return bsplineDerivs<N,3>(x, &xnodes[0], xnodes.size(), values);
        default:
            throw std::invalid_argument("nonzeroComponents: invalid order of derivative");
    }
}

template<int N>
double BsplineInterpolator1d<N>::interpolate(
    const double x, const std::vector<double> &amplitudes, const unsigned int derivOrder) const
{
    if(amplitudes.size() != numComp)
        throw std::length_error("interpolate: invalid size of amplitudes array");
    if(derivOrder > N)
        return 0;
    if(derivOrder > 3)
        throw std::invalid_argument("interpolate: invalid order of derivative");
    double bspl[N+1];
    unsigned int leftInd = derivOrder==0 ?
        bsplineValues<N>  (x, &xnodes[0], xnodes.size(), bspl) : derivOrder==1 ?
        bsplineDerivs<N,1>(x, &xnodes[0], xnodes.size(), bspl) : derivOrder==2 ?
        bsplineDerivs<N,2>(x, &xnodes[0], xnodes.size(), bspl) :
        bsplineDerivs<N,3>(x, &xnodes[0], xnodes.size(), bspl);
    double val=0;
    for(int i=0; i<=N; i++)
        val += bspl[i] * amplitudes[i+leftInd];
    return val;
}

template<int N>
void BsplineInterpolator1d<N>::eval(const double* x, double values[]) const
{
    std::fill(values, values+numComp, 0.);
    double bspl[N+1];
    unsigned int leftInd = bsplineValues<N>(*x, &xnodes[0], xnodes.size(), bspl);
    for(int i=0; i<=N; i++)
        values[i+leftInd] = bspl[i];
}

template<int N>
std::vector<double> BsplineInterpolator1d<N>::deriv(const std::vector<double> &amplitudes) const
{    
    if(amplitudes.size() != numComp)
        throw std::length_error("deriv: invalid size of amplitudes array");
    std::vector<double> result(amplitudes);
    for(int i=0; i<(int)numComp-1; i++) {
        result[i] = N * (result[i+1] - result[i]) * denom(&xnodes[0], xnodes.size(), i-N+1, i+1);
    }
    result.pop_back();
    return result;
}

template<int N>
std::vector<double> BsplineInterpolator1d<N>::antideriv(const std::vector<double> &amplitudes) const
{    
    if(amplitudes.size() != numComp)
        throw std::length_error("antideriv: invalid size of amplitudes array");
    std::vector<double> result(numComp+1);
    for(int i=0; i<(int)numComp; i++) {
        result[i+1] = result[i] + amplitudes[i] / (N+1) / denom(&xnodes[0], xnodes.size(), i-N, i+1);
    }
    return result;
}

template<int N>
double BsplineInterpolator1d<N>::integrate(double x1, double x2,
    const std::vector<double> &amplitudes, int n) const
{
    if(amplitudes.size() != numComp)
        throw std::length_error("integrate: invalid size of amplitudes array");
    double sign = 1.;
    if(x1>x2) {  // swap limits of integration
        double tmp=x2;
        x2 = x1;
        x1 = tmp;
        sign = -1.;
    }

    // find out the min/max indices of grid segments that contain the integration interval
    const double* xgrid = &xnodes.front();
    const int Ngrid = xnodes.size();
    int i1 = std::max<int>(binSearch(x1, xgrid, Ngrid), 0);
    int i2 = std::min<int>(binSearch(x2, xgrid, Ngrid), Ngrid-2);

    // B-spline of degree N is a piecewise polynomial of degree N, thus to compute the integral
    // of B-spline times x^n on each grid segment, it is sufficient to employ a Gauss-Legendre
    // quadrature rule with the number of nodes = floor((N+n)/2)+1.
    const int NnodesGL = (N+n)/2+1;
    std::vector<double> glnodes(NnodesGL), glweights(NnodesGL);
    prepareIntegrationTableGL(0, 1, NnodesGL, &glnodes[0], &glweights[0]);

    // loop over segments
    double result = 0;
    for(int i=i1; i<=i2; i++) {
        double X1 = i==i1 ? x1 : xgrid[i];
        double X2 = i==i2 ? x2 : xgrid[i+1];
        double bspl[N+1];
        for(int k=0; k<NnodesGL; k++) {
            const double x = X1 + (X2 - X1) * glnodes[k];
            // evaluate the possibly non-zero functions and keep track of the index of the leftmost one
            int leftInd = bsplineValues<N>(x, xgrid, Ngrid, bspl);
            // add the contribution of this GL point to the integral of x^n * \sum A_j B_j(x),
            // where the index j runs from leftInd to leftInd+N
            double fval = 0;
            for(int b=0; b<=N; b++)
                fval += bspl[b] * amplitudes[b+leftInd];
            result += (X2-X1) * fval * glweights[k] * pow(x, n);
        }
    }
    return result * sign;
}

// force template instantiations for several values of N
template class BsplineInterpolator1d<0>;
template class BsplineInterpolator1d<1>;
template class BsplineInterpolator1d<2>;
template class BsplineInterpolator1d<3>;

// ------ Finite-element features of B-splines ------ //

template<int N>
FiniteElement1d<N>::FiniteElement1d(const std::vector<double>& xnodes) :
    interp(xnodes)
{
    const unsigned int
    gridSize = (xnodes.size()-1) * GLORDER,  // total # of points on the integration grid
    numFunc  = N+1,   // # of nonzero basis functions on each segment
    numDeriv = N+1;   // # of nontrivial derivatives including the function itself
    // prepare table for Gauss-Legendre integration with GLORDER points per each segment of the input grid
    double glnodes[GLORDER], glweights[GLORDER];
    prepareIntegrationTableGL(0, 1, GLORDER, glnodes, glweights);
    integrNodes.resize(gridSize);
    integrWeights.resize(gridSize);
    bsplValues.resize(numFunc * gridSize * numDeriv);
    for(unsigned int p=0; p<gridSize; p++) {
        // store the nodes and weights of GL quadrature at the given node of integration grid
        unsigned int k   = p / GLORDER, n = p % GLORDER; // index of the segment of the input grid
        double dx        = xnodes[k+1] - xnodes[k];      // width of this segment
        integrNodes  [p] = xnodes[k] + dx * glnodes[n];  // point inside this segment
        integrWeights[p] = dx * glweights[n];            // its weight in the overall quadrature rule
        // collect the values and derivatives of all B-spline functions at the nodes of integration grid
        for(unsigned int d=0; d<numDeriv; d++) {
            unsigned int index =  // index of the first nontrivial basis function at the current point
            interp.nonzeroComponents(integrNodes[p], d, &bsplValues[numFunc * (d * gridSize + p)]);
            assert(index == k);
        }
    }
}

template<int N>
std::vector<double> FiniteElement1d<N>::computeProjVector(
    const std::vector<double>& fncValues, unsigned int derivOrder) const
{
    const unsigned int gridSize = integrNodes.size(), numFunc = N+1, numBasisFnc = interp.numValues();
    if(fncValues.size() != gridSize)
        throw std::length_error("computeProjVector: invalid size of input array");
    std::vector<double> result(numBasisFnc);
    for(unsigned int p=0; p<gridSize; p++) {
        // value of input function times the weight of Gauss-Legendre quadrature at point p
        double fw = fncValues[p] * integrWeights[p];
        // index of the first out of numFunc basis functions pre-computed at the current point
        unsigned int index = p / GLORDER;
        assert(index + numFunc <= numBasisFnc);
        for(unsigned int k=0; k<numFunc; k++)
            result[index + k] += fw * bsplValues[numFunc * (derivOrder * gridSize + p) + k];
    }
    return result;
}

template<int N>
BandMatrix<double> FiniteElement1d<N>::computeProjMatrix(
    const std::vector<double>& fncValues, unsigned int derivOrderP, unsigned int derivOrderQ) const
{
    const unsigned int
    gridSize    = integrNodes.size(),  // # of points in the integration grid
    numFunc     = N+1,                 // # of nontrivial basis functions at each point
    numBasisFnc = interp.numValues();  // total number of basis functions (heigth of band matrix)
    bool empty  = fncValues.empty();   // whether the function f(x) is provided (otherwise take f=1)
    if(!empty && fncValues.size() != gridSize)
        throw std::length_error("computeProjMatrix: invalid size of input array");
    BandMatrix<double> mat(numBasisFnc, N, 0.);
    for(unsigned int p=0; p<gridSize; p++) {
        // value of input function (if provided) times the weight of Gauss-Legendre quadrature at point p
        double fw = empty? integrWeights[p] : fncValues[p] * integrWeights[p];
        // index of the first out of numFunc basis functions pre-computed at the current point p
        unsigned int index = p / GLORDER;
        // basis functions B_i with indices  index <= i < index+numFunc  are nonzero at the given point,
        // so the band matrix elements A_{ij} are nonzero only if both i and j are in this range
        for(unsigned int ki=0; ki<numFunc; ki++) {
            // value or derivative of the basis function at i-th row, pre-multiplied with f_p w_p
            double B_i = bsplValues[numFunc * (derivOrderP * gridSize + p) + ki] * fw;
            for(unsigned int kj=0; kj<numFunc; kj++) {
                // value or derivative of the basis function at j-th column
                double B_j = bsplValues[numFunc * (derivOrderQ * gridSize + p) + kj];
                // accumulate the contribution of the point p to the integral of B_i B_j
                mat(ki+index, kj+index) += B_i * B_j;
            }
        }
    }
    return mat;
}

template<int N>
std::vector<double> FiniteElement1d<N>::computeAmplitudes(const IFunction& F) const
{
    const unsigned int gridSize = integrNodes.size();
    // collect the function values at the nodes of integration grid
    std::vector<double> fncValues(gridSize);
    for(unsigned int p=0; p<gridSize; p++)
        fncValues[p] = F(integrNodes[p]);
    // compute the projection integrals and solve the linear equation to find the amplitudes
    return solveBand(computeProjMatrix(), computeProjVector(fncValues));
}

// force template instantiations for several values of N
template class FiniteElement1d<0>;
template class FiniteElement1d<1>;
template class FiniteElement1d<2>;
template class FiniteElement1d<3>;


// ------ INTERPOLATION IN 2D ------ //

BaseInterpolator2d::BaseInterpolator2d(
    const std::vector<double>& xgrid, const std::vector<double>& ygrid,
    const Matrix<double>& fvalues) :
    xval(xgrid), yval(ygrid), fval(fvalues.data(), fvalues.data() + fvalues.size())
{
    const size_t xsize = xgrid.size();
    const size_t ysize = ygrid.size();
    if(xsize<2 || ysize<2)
        throw std::invalid_argument(
            "Error in 2d interpolator initialization: number of nodes should be >=2 in each direction");
    if(fvalues.rows() != xsize)
        throw std::length_error(
            "Error in 2d interpolator initialization: x and f array lengths differ");
    if(fvalues.cols() != ysize)
        throw std::length_error(
            "Error in 2d interpolator initialization: y and f array lengths differ");
}

// ------- Bilinear interpolation in 2d ------- //

void LinearInterpolator2d::evalDeriv(const double x, const double y,
     double *z, double *z_x, double *z_y, double *z_xx, double *z_xy, double *z_yy) const
{
    if(fval.empty())
        throw std::length_error("Empty 2d interpolator");
    // 2nd derivatives are always zero
    if(z_xx)
        *z_xx = 0;
    if(z_xy)
        *z_xy = 0;
    if(z_yy)
        *z_yy = 0;
    const int
        nx  = xval.size(),
        ny  = yval.size(),
        xi  = binSearch(x, &xval.front(), nx),
        yi  = binSearch(y, &yval.front(), ny),
        // indices of corner nodes in the flattened 2d array
        ill = xi * ny + yi, // xlow,ylow
        ilu = ill + 1,      // xlow,yupp
        iul = ill + ny,     // xupp,ylow
        iuu = iul + 1;      // xupp,yupp
    // no interpolation outside the 2d grid
    if(xi<0 || xi>=nx-1 || yi<0 || yi>=ny-1) {
        if(z)
            *z    = NAN;
        if(z_x)
            *z_x  = NAN;
        if(z_y)
            *z_y  = NAN;
        return;
    }
    const double
        zlowlow = fval[ill],
        zlowupp = fval[ilu],
        zupplow = fval[iul],
        zuppupp = fval[iuu],
        // width and height of the grid cell
        dx = xval[xi+1] - xval[xi],
        dy = yval[yi+1] - yval[yi],
        // relative positions within the grid cell [0:1], in units of grid cell size
        t = (x - xval[xi]) / dx,
        u = (y - yval[yi]) / dy;
    if(z)
        *z = (1-t)*(1-u) * zlowlow + t*(1-u) * zupplow + (1-t)*u * zlowupp + t*u * zuppupp;
    if(z_x)
        *z_x = (-(1-u) * zlowlow + (1-u) * zupplow - u * zlowupp + u * zuppupp) / dx;
    if(z_y)
        *z_y = (-(1-t) * zlowlow - t * zupplow + (1-t) * zlowupp + t * zuppupp) / dy;
}


//------------ 2D CUBIC SPLINE -------------//

CubicSpline2d::CubicSpline2d(const std::vector<double>& xgrid, const std::vector<double>& ygrid,
    const Matrix<double>& fvalues,
    double deriv_xmin, double deriv_xmax, double deriv_ymin, double deriv_ymax) :
    BaseInterpolator2d(xgrid, ygrid, fvalues),
    fx (fvalues.size()),
    fy (fvalues.size()),
    fxy(fvalues.size())
{
    const size_t xsize = xgrid.size();
    const size_t ysize = ygrid.size();
    std::vector<double> tmpvalues(ysize);
    // step 1. for each x_i, construct cubic splines for f(x_i, y) in y and assign df/dy at grid nodes
    for(size_t i=0; i<xsize; i++) {
        for(size_t j=0; j<ysize; j++)
            tmpvalues[j] = fval[i * ysize + j];
        tmpvalues = constructCubicSpline(yval, tmpvalues, deriv_ymin, deriv_ymax);
        for(size_t j=0; j<ysize; j++)
            fy[i * ysize + j] = tmpvalues[j];
    }
    tmpvalues.resize(xsize);
    // step 2. for each y_j, construct cubic splines for f(x, y_j) in x and assign df/dx at grid nodes
    for(size_t j=0; j<ysize; j++) {
        for(size_t i=0; i<xsize; i++)
            tmpvalues[i] = fval[i * ysize + j];
        tmpvalues = constructCubicSpline(xval, tmpvalues, deriv_xmin, deriv_xmax);
        for(size_t i=0; i<xsize; i++)
            fx[i * ysize + j] = tmpvalues[i];
        // step 3. assign the mixed derivative d2f/dxdy:
        // if derivs at the boundary are specified and constant, 2nd deriv must be zero
        if( (j==0 && isFinite(deriv_ymin)) || (j==ysize-1 && isFinite(deriv_ymax)) ) {
            for(size_t i=0; i<xsize; i++)
                fxy[i * ysize + j] = 0.;
        } else {
            // otherwise construct cubic splines for df/dy(x,y_j) in x and assign d2f/dydx
            for(size_t i=0; i<xsize; i++)
                tmpvalues[i] = fy[i * ysize + j];
            tmpvalues = constructCubicSpline(xval, tmpvalues,
                isFinite(deriv_xmin) ? 0. : NAN, isFinite(deriv_xmax) ? 0. : NAN);
            for(size_t i=0; i<xsize; i++)
                fxy[i * ysize + j] = tmpvalues[i];
        }
    }
}

void CubicSpline2d::evalDeriv(const double x, const double y,
    double *z, double *z_x, double *z_y, double *z_xx, double *z_xy, double *z_yy) const
{
    if(fval.empty())
        throw std::length_error("Empty 2d spline");
    const int
        nx = xval.size(),
        ny = yval.size(),
        // indices of grid cell in x and y
        xi = binSearch(x, &xval.front(), nx),
        yi = binSearch(y, &yval.front(), ny),
        // indices in flattened 2d arrays:
        ill = xi * ny + yi, // xlow,ylow
        ilu = ill + 1,      // xlow,yupp
        iul = ill + ny,     // xupp,ylow
        iuu = iul + 1;      // xupp,yupp
    if(xi<0 || xi>=nx-1 || yi<0 || yi>=ny-1) {
        if(z)
            *z    = NAN;
        if(z_x)
            *z_x  = NAN;
        if(z_y)
            *z_y  = NAN;
        if(z_xx)
            *z_xx = NAN;
        if(z_xy)
            *z_xy = NAN;
        if(z_yy)
            *z_yy = NAN;
        return;
    }
    const double
        // coordinates of corner points
        xlow = xval[xi],
        xupp = xval[xi+1],
        ylow = yval[yi],
        yupp = yval[yi+1],
        // values and derivatives for the intermediate Hermite splines
        flow  [4] = { fval[ill], fval[iul], fx [ill], fx [iul] },
        fupp  [4] = { fval[ilu], fval[iuu], fx [ilu], fx [iuu] },
        dflow [4] = { fy  [ill], fy  [iul], fxy[ill], fxy[iul] },
        dfupp [4] = { fy  [ilu], fy  [iuu], fxy[ilu], fxy[iuu] };
    double F  [4];  // {   f    (xlow, y),   f    (xupp, y),  df/dx   (xlow, y),  df/dx   (xupp, y) }
    double dF [4];  // {  df/dy (xlow, y),  df/dy (xupp, y), d2f/dxdy (xlow, y), d2f/dxdy (xupp, y) }
    double d2F[4];  // { d2f/dy2(xlow, y), d2f/dy2(xupp, y), d3f/dxdy2(xlow, y), d3f/dxdy2(xupp, y) }
    bool der  = z_y!=NULL || z_xy!=NULL;
    bool der2 = z_yy!=NULL;
    // intermediate interpolation along y direction
    evalCubicSplines<4> (y, ylow, yupp, flow, fupp, dflow, dfupp,
        /*output*/ F, der? dF : NULL, der2? d2F : NULL);
    // final interpolation along x direction
    evalCubicSplines<1> (x, xlow, xupp, &F[0], &F[1], &F[2], &F[3],
        /*output*/ z, z_x, z_xx);
    if(der)
        evalCubicSplines<1> (x, xlow, xupp, &dF[0], &dF[1], &dF[2], &dF[3],
            /*output*/ z_y, z_xy, NULL);
    if(der2)
        evalCubicSplines<1> (x, xlow, xupp, &d2F[0], &d2F[1], &d2F[2], &d2F[3],
            /*output*/ z_yy, NULL, NULL);
}


//------------ 2D QUINTIC SPLINE -------------//

QuinticSpline2d::QuinticSpline2d(const std::vector<double>& xgrid, const std::vector<double>& ygrid,
    const Matrix<double>& fvalues, const Matrix<double>& dfdx, const Matrix<double>& dfdy) :
    BaseInterpolator2d(xgrid, ygrid, fvalues),
    fx   (dfdx.data(), dfdx.data() + dfdx.size()),
    fy   (dfdy.data(), dfdy.data() + dfdy.size()),
    fxx  (fvalues.size()),
    fxy  (fvalues.size()),
    fyy  (fvalues.size()),
    fxxy (fvalues.size()),
    fxyy (fvalues.size()),
    fxxyy(fvalues.size())
{
    const size_t xsize = xgrid.size();
    const size_t ysize = ygrid.size();
    if(dfdx.rows() != xsize || dfdy.rows() != xsize || dfdx.cols() != ysize || dfdy.cols() != ysize)
        throw std::length_error("QuinticSpline2d: invalid size of derivatives matrix");

    // temporary arrays for 1d splines
    std::vector<double> t, tx, ty, txx, txy, tyy, txxy, txyy;
    // additional safety measures: if the derivative df/dx is zero for all y and a particular column x[i],
    // then the higher mixed derivatives d^2f/dxdy and d^3f/dx^2 dy must be zero for all y in this column;
    // similarly for df/dy. The two arrays keep track of these conditions for each row/column.
    std::vector<char> fxzero(xsize, true), fyzero(ysize, true);

    // step 1. for each y_j, construct:
    // a) 1d quintic spline for f in x, and record d^2f/dx^2;
    // b) 1d cubic spline for df/dy in x, store d^2f/dxdy and d^3f/dx^2 dy
    //    (the latter only for the boundary columns, j=0 or j=ysize-1)
    t. resize(xsize);
    tx.resize(xsize);
    ty.resize(xsize);
    for(size_t j=0; j<ysize; j++) {
        for(size_t i=0; i<xsize; i++) {
            t [i] = fval[i * ysize + j];
            tx[i] = fx  [i * ysize + j];
            ty[i] = fy  [i * ysize + j];
            fyzero[j] &= ty[i]==0;
        }
        txx = constructQuinticSpline(xval, t, tx);
        txy = constructCubicSpline(xval, ty);
        for(size_t i=0; i<xsize; i++) {
            fxx[i * ysize + j] = txx[i];  // 1a.
            fxy[i * ysize + j] = txy[i];  // 1b.
            if(j==0 || j==ysize-1) {
                size_t ii = std::min(i, xsize-2);
                evalCubicSplines<1>( xval[i], xval[ii], xval[ii+1],
                    &ty[ii], &ty[ii+1], &txy[ii], &txy[ii+1],
                    /*output*/NULL, NULL, &fxxy[i * ysize + j]);
            }
        }
    }

    // step 2. for each x_i, construct:
    // a) quintic spline for f in y, and store d^2f/dy^2;
    // b) cubic spline for df/dx in y, and record d^2f/dxdy (combine with the estimate obtained at 1b),
    //    and d^3f / dx dy^2  (for the boundary rows only, i=0 or i=xsize-1);
    // c) cubic spline for d^2f/dx^2 in y, and record d^3f / dx^2 dy, d^4f / dx^2 dy^2
    //    (except the boundary columns j=0 and j=ysize-1)
    t.  resize(ysize);
    tx. resize(ysize);
    ty. resize(ysize);
    txx.resize(ysize);
    for(size_t i=0; i<xsize; i++) {
        for(size_t j=0; j<ysize; j++) {
            t  [j] = fval[i * ysize + j];
            tx [j] = fx  [i * ysize + j];
            ty [j] = fy  [i * ysize + j];
            txx[j] = fxx [i * ysize + j];
            fxzero[i] &= tx[j]==0;
        }
        tyy = constructQuinticSpline(yval, t, ty);
        txy = constructCubicSpline(yval, tx);
        txxy= constructCubicSpline(yval, txx);
        for(size_t j=0; j<ysize; j++) {
            // 2a. record d2f/dy2
            fyy[i * ysize + j] = tyy[j];
            // 2b. handle the mixed derivative d2f/dxdy
            double f2, f3;
            size_t jj = std::min(j, ysize-2);
            evalCubicSplines<1>(yval[j], yval[jj], yval[jj+1],
                &tx[jj], &tx[jj+1], &txy[jj], &txy[jj+1], /*output*/NULL, &f2, &f3);
            // we now have two different estimates: from the spline for df/dy as a function of x
            // (obtained in the step 1b), and from the spline for df/dx as a function of y (obtained now).
            // the first one is not accurate at the boundary nodes i=0, i=xsize-1,
            // while the second one is not accurate at the boundary nodes j=0, j=ysize-1;
            // thus on the grid edges we retain only the more accurate one,
            // while for the interior nodes or for the four corners we use an average of them
            // (but if df/dx=0 for all y or df/dy=0 for all x, then the mixed deriv should also be zero)
            if((i==0 || i==xsize-1) && j!=0 && j!=ysize-1) {
                fxy [i * ysize + j] = f2;
                fxyy[i * ysize + j] = f3;
            }
            if((j!=0 && j!=ysize-1) || ((i==0 || i==xsize-1) && (j==0 || j==ysize-1))) {
                double f1 = fxy[i * ysize + j];
                fxy[i * ysize + j] = fxzero[i] || fyzero[j] ? 0. : (f1 + f2) * 0.5;
            }
            // 2c. assign d3f/dx2dy and d4f/dx2dy2 for all columns except the boundaries
            // (j=0 or j=ysize-1), where it is expected to be inaccurate
            if(j!=0 && j!=ysize-1) {
                evalCubicSplines<1>(yval[j], yval[jj], yval[jj+1],
                    &txx[jj], &txx[jj+1], &txxy[jj], &txxy[jj+1],
                    /*output*/NULL, &fxxy[i * ysize + j], &fxxyy[i * ysize + j]);
                if(fyzero[j])   // if df/dy=0 for all x, then its further derivs by x must be zero too
                    fxxy[i * ysize + j] = 0.;
            }
        }
    }

    // step 3. for each y_j, construct:
    // c) cubic spline for d^2f/dy^2 in x, and record d^3f / dx dy^2, d^4f / dx^2 dy^2
    tyy.resize(xsize);
    for(size_t j=0; j<ysize; j++) {
        for(size_t i=0; i<xsize; i++)
            tyy[i] = fyy[i * ysize + j];
        txyy = constructCubicSpline(xval, tyy);
        for(size_t i=1; i<xsize-1; i++) {
            double f4;
            // assign d3f/dxdy2 for all rows except the boundaries (i=0 or i=xsize-1)
            size_t ii = std::min(i, xsize-2);
            evalCubicSplines<1>(xval[i], xval[ii], xval[ii+1],
                &tyy[ii], &tyy[ii+1], &txyy[ii], &txyy[ii+1],
                /*output*/NULL, &fxyy[i * ysize + j], &f4);
            // assign d4f/dx2dy2 or take the symmetric average with the one computed previously
            fxxyy[i * ysize + j] = j==0 || j==ysize-1 ? f4 : (fxxyy[i * ysize + j] + f4) * 0.5;
            if(fxzero[i])
                fxyy[i * ysize + j] = 0.;
        }
    }
}

void QuinticSpline2d::evalDeriv(const double x, const double y,
    double* z, double* z_x, double* z_y,
    double* z_xx, double* z_xy, double* z_yy) const
{
    if(fval.empty())
        throw std::length_error("Empty 2d spline");
    const int
        nx = xval.size(),
        ny = yval.size(),
        // indices of grid cell in x and y
        xi = binSearch(x, &xval.front(), nx),
        yi = binSearch(y, &yval.front(), ny),
        // indices in flattened 2d arrays:
        ill = xi * ny + yi, // xlow,ylow
        ilu = ill + 1,      // xlow,yupp
        iul = ill + ny,     // xupp,ylow
        iuu = iul + 1;      // xupp,yupp
    if(xi<0 || xi>=nx-1 || yi<0 || yi>=ny-1) {
        if(z)
            *z    = NAN;
        if(z_x)
            *z_x  = NAN;
        if(z_y)
            *z_y  = NAN;
        if(z_xx)
            *z_xx = NAN;
        if(z_xy)
            *z_xy = NAN;
        if(z_yy)
            *z_yy = NAN;
        return;
    }
    bool der  = z_y!=NULL || z_xy!=NULL;
    bool der2 = z_yy!=NULL;
    const double
        // coordinates of corner points
        xlow = xval[xi],
        xupp = xval[xi+1],
        ylow = yval[yi],
        yupp = yval[yi+1],
        // values and derivatives for the intermediate splines
        fl [6] = { fval[ill], fval[iul], fx  [ill], fx  [iul], fxx  [ill], fxx  [iul] },
        fu [6] = { fval[ilu], fval[iuu], fx  [ilu], fx  [iuu], fxx  [ilu], fxx  [iuu] },
        f1l[6] = { fy  [ill], fy  [iul], fxy [ill], fxy [iul], fxxy [ill], fxxy [iul] },
        f1u[6] = { fy  [ilu], fy  [iuu], fxy [ilu], fxy [iuu], fxxy [ilu], fxxy [iuu] },
        f2l[6] = { fyy [ill], fyy [iul], fxyy[ill], fxyy[iul], fxxyy[ill], fxxyy[iul] },
        f2u[6] = { fyy [ilu], fyy [iuu], fxyy[ilu], fxyy[iuu], fxxyy[ilu], fxxyy[iuu] };
    // compute intermediate splines
    double
        F  [6],  // {   f    (xlow/upp, y),  df/dx   (xl/u, y), d2f/dx2   (xl/u, y) }
        dF [6],  // {  df/dy (xlow/upp, y), d2f/dxdy (xl/u, y), d3f/dx2dy (xl/u, y) }
        d2F[6];  // { d2f/dy2(xlow/upp, y), d3f/dxdy2(xl/u, y), d4f/dx2dy2(xl/u, y) }
    evalQuinticSplines<6> (y, ylow, yupp, fl, fu, f1l, f1u, f2l, f2u,
            /*output*/ F, der? dF : NULL, der2? d2F : NULL);
    // compute and output requested values and derivatives
    evalQuinticSplines<1> (x, xlow, xupp, &F[0], &F[1], &F[2], &F[3], &F[4], &F[5],
            /*output*/ z, z_x, z_xx);
    if(z_y || z_xy)
        evalQuinticSplines<1> (x, xlow, xupp, &dF[0], &dF[1], &dF[2], &dF[3], &dF[4], &dF[5],
            /*output*/ z_y, z_xy, NULL);
    if(z_yy)
        evalQuinticSplines<1> (x, xlow, xupp, &d2F[0], &d2F[1], &d2F[2], &d2F[3], &d2F[4], &d2F[5],
            /*output*/ z_yy, NULL, NULL);
}


// ------- Interpolation in 3d ------- //

LinearInterpolator3d::LinearInterpolator3d(const std::vector<double>& xnodes,
    const std::vector<double>& ynodes, const std::vector<double>& znodes,
    const std::vector<double>& fvalues) :
    xval(xnodes), yval(ynodes), zval(znodes), fval(fvalues)
{
    const int nx = xval.size(), ny = yval.size(), nz = zval.size();
    const unsigned int nval = nx*ny*nz;   // total number of nodes in the 3d grid
    if(nx < 2 || ny < 2 || nz < 2 || fvalues.size() != nval)
        throw std::length_error("LinearInterpolator3d: invalid grid sizes");
}

double LinearInterpolator3d::value(double x, double y, double z) const
{
    const int
    nx = xval.size(),
    ny = yval.size(),
    nz = zval.size(),
    // indices of grid cell in x, y and z
    xi = binSearch(x, &xval.front(), nx),
    yi = binSearch(y, &yval.front(), ny),
    zi = binSearch(z, &zval.front(), nz),
    il = (xi * ny + yi) * nz + zi,
    iu = il + ny * nz;
    if(xi<0 || xi>=nx || yi<0 || yi>=ny || zi<0 || zi>=nz)
        return NAN;
    const double
    // relative positions within the grid cell [0:1], in units of grid cell size
    offx = (x - xval[xi]) / (xval[xi+1] - xval[xi]),
    offy = (y - yval[yi]) / (yval[yi+1] - yval[yi]),
    offz = (z - zval[zi]) / (zval[zi+1] - zval[zi]),
    // values of function at 8 corners
    flll = fval[il],          // xlow,ylow,zlow
    fllu = fval[il + 1],      // xlow,ylow,zupp
    flul = fval[il + nz],     // xlow,yupp,zlow
    fluu = fval[il + nz + 1], // xlow,yupp,zupp
    full = fval[iu],          // xupp,ylow,zlow
    fulu = fval[iu + 1],      // xupp,ylow,zupp
    fuul = fval[iu + nz],     // xupp,yupp,zlow
    fuuu = fval[iu + nz + 1]; // xupp,yupp,zupp
    return
        ( (1-offy) * ( (1-offz) * flll + offz * fllu ) +
             offy  * ( (1-offz) * flul + offz * fluu ) ) * (1-offx) +
        ( (1-offy) * ( (1-offz) * full + offz * fulu ) +
             offy  * ( (1-offz) * fuul + offz * fuuu ) ) * offx;
}


CubicSpline3d::CubicSpline3d(const std::vector<double>& xnodes, const std::vector<double>& ynodes,
    const std::vector<double>& znodes, const std::vector<double>& fvalues) :
    xval(xnodes), yval(ynodes), zval(znodes)
{
    const int nx = xval.size(), ny = yval.size(), nz = zval.size();
    const unsigned int nval = nx*ny*nz,   // total number of nodes in the 3d grid
        nampl = (nx+2)*(ny+2)*(nz+2);     // or the number of amplitudes of B-splines
    if(nx < 2 || ny < 2 || nz < 2 ||
        !(fvalues.size() == nval || fvalues.size() == nampl) )
        throw std::length_error("CubicSpline3d: invalid grid sizes");
    fval.resize(nval);
    fx  .resize(nval);
    fy  .resize(nval);
    fz  .resize(nval);
    fxy .resize(nval);
    fxz .resize(nval);
    fyz .resize(nval);
    fxyz.resize(nval);

    if(fvalues.size() == nampl) {
        // assume that the input array contained amplitudes of a 3d cubic B-spline
        const std::vector<double>* nodes[3] = {&xval, &yval, &zval};
        Matrix<double> values[3], derivs[3];
        std::vector<int> leftInd[3];
        // collect the values and derivs of all basis functions at each grid node in each dimension
        for(int d=0; d<3; d++) {
            unsigned int Ngrid = nodes[d]->size();
            values[d] = Matrix<double>(Ngrid, 4);
            derivs[d] = Matrix<double>(Ngrid, 4);
            leftInd[d].resize(Ngrid);
            const double* arr = &(nodes[d]->front());
            for(unsigned int n=0; n<Ngrid; n++) {
                leftInd[d][n] = bsplineValues<3>(arr[n], arr, Ngrid, &values[d](n, 0));
                bsplineDerivs<3,1>(arr[n], arr, Ngrid, &derivs[d](n, 0));
            }
        }
        for(int xi=0; xi<nx; xi++)
            for(int yi=0; yi<ny; yi++)
                for(int zi=0; zi<nz; zi++) {
                    int K = (xi * ny + yi) * nz + zi;
                    for(int i=0; i<=3; i++)
                        for(int j=0; j<=3; j++)
                            for(int k=0; k<=3; k++) {
                                double a = fvalues[ ((i+leftInd[0][xi]) * (ny+2) + j+leftInd[1][yi]) *
                                    (nz+2) + k+leftInd[2][zi] ];
                                fval[K] += a * values[0](xi,i) * values[1](yi,j) * values[2](zi,k);
                                fx  [K] += a * derivs[0](xi,i) * values[1](yi,j) * values[2](zi,k);
                                fy  [K] += a * values[0](xi,i) * derivs[1](yi,j) * values[2](zi,k);
                                fz  [K] += a * values[0](xi,i) * values[1](yi,j) * derivs[2](zi,k);
                                fxy [K] += a * derivs[0](xi,i) * derivs[1](yi,j) * values[2](zi,k);
                                fxz [K] += a * derivs[0](xi,i) * values[1](yi,j) * derivs[2](zi,k);
                                fyz [K] += a * values[0](xi,i) * derivs[1](yi,j) * derivs[2](zi,k);
                                fxyz[K] += a * derivs[0](xi,i) * derivs[1](yi,j) * derivs[2](zi,k);
                            }
                }
        return;
    }

    // otherwise the input array contains the values of function at 3d grid nodes
    fval = fvalues;

    std::vector<double> tmpx(nx), tmpy(ny), tmpz(nz), tmpxy(nx), tmpxz(nx), tmpyz, tmpxyz;
    // step 1. construct splines from function values and store the first derivatives at grid nodes
    // a. for each y_j,z_k construct cubic splines for f(x, y_j, z_k) in x and store df/fx
    for(int j=0; j<ny; j++)
        for(int k=0; k<nz; k++) {
            for(int i=0; i<nx; i++)
                tmpx[i] = fval[ (i*ny + j) * nz + k ];
            tmpx = constructCubicSpline(xval, tmpx);
            for(int i=0; i<nx; i++)
                fx[ (i*ny + j) * nz + k ] = tmpx[i];
        }
    // b. for each x_i,z_k construct cubic splines for f(x_i, y, z_k) in y and store df/fy
    for(int i=0; i<nx; i++)
        for(int k=0; k<nz; k++) {
            for(int j=0; j<ny; j++)
                tmpy[j] = fval[ (i*ny + j) * nz + k ];
            tmpy = constructCubicSpline(yval, tmpy);
            for(int j=0; j<ny; j++)
                fy[ (i*ny + j) * nz + k ] = tmpy[j];
        }
    // c. for each x_i,y_j construct cubic splines for f(x_i, y_j, z) in z and store df/fz
    for(int i=0; i<nx; i++)
        for(int j=0; j<ny; j++) {
            tmpz.assign(fval.begin() + (i*ny + j) * nz, fval.begin() + (i*ny + j+1) * nz);
            tmpz = constructCubicSpline(zval, tmpz);
            for(int k=0; k<nz; k++)
                fz[ (i*ny + j) * nz + k ] = tmpz[k];
        }

    // step 2. construct splines from first derivatives and store mixed second derivatives at grid nodes
    // a,b:  compute d2f/dxdy, d2f/dxdz
    for(int j=0; j<ny; j++)
        for(int k=0; k<nz; k++) {
            for(int i=0; i<nx; i++) {
                tmpxy[i] = fy[ (i*ny + j) * nz + k ];
                tmpxz[i] = fz[ (i*ny + j) * nz + k ];
            }
            tmpxy = constructCubicSpline(xval, tmpxy);
            tmpxz = constructCubicSpline(xval, tmpxz);
            for(int i=0; i<nx; i++) {
                fxy[ (i*ny + j) * nz + k ] = tmpxy[i];
                fxz[ (i*ny + j) * nz + k ] = tmpxz[i];
            }
        }
    // 2c:  compute d2f/dydz  and  step 3: compute d3f/dxdydz
    for(int i=0; i<nx; i++)
        for(int j=0; j<ny; j++) {
            tmpyz.assign (fy .begin() + (i*ny + j) * nz, fy .begin() + (i*ny + j+1) * nz);
            tmpxyz.assign(fxy.begin() + (i*ny + j) * nz, fxy.begin() + (i*ny + j+1) * nz);
            tmpyz  = constructCubicSpline(zval, tmpyz);
            tmpxyz = constructCubicSpline(zval, tmpxyz);
            for(int k=0; k<nz; k++) {
                fyz [ (i*ny + j) * nz + k ] = tmpyz[k];
                fxyz[ (i*ny + j) * nz + k ] = tmpxyz[k];
            }
        }
}

double CubicSpline3d::value(double x, double y, double z) const
{
    const int
    nx = xval.size(),
    ny = yval.size(),
    nz = zval.size(),
    // indices of grid cell in x, y and z
    xi = binSearch(x, &xval.front(), nx),
    yi = binSearch(y, &yval.front(), ny),
    zi = binSearch(z, &zval.front(), nz);
    if(xi<0 || xi>=nx || yi<0 || yi>=ny || zi<0 || zi>=nz)
        return NAN;
    const int
    // indices in flattened 3d arrays:
    illl = (xi * ny + yi) * nz + zi, // xlow,ylow,zlow
    illu = illl + 1,                 // xlow,ylow,zupp
    ilul = illl + nz,                // xlow,yupp,zlow
    iluu = ilul + 1,                 // xlow,yupp,zupp
    iull = illl + ny * nz,           // xupp,ylow,zlow
    iulu = iull + 1,                 // xupp,ylow,zupp
    iuul = iull + nz,                // xupp,yupp,zlow
    iuuu = iuul + 1;                 // xupp,yupp,zupp
    const double
    // coordinates of corner points
    xlow = xval[xi],
    xupp = xval[xi+1],
    ylow = yval[yi],
    yupp = yval[yi+1],
    zlow = zval[zi],
    zupp = zval[zi+1],
    // 1st stage: interpolate along x axis to obtain  f, f_y, f_z, f_yz  at four corners of the y-z cell
    fl [16] = { fval[illl], fval[illu], fz  [illl], fz  [illu],
                fval[ilul], fval[iluu], fz  [ilul], fz  [iluu],
                fy  [illl], fy  [illu], fyz [illl], fyz [illu],
                fy  [ilul], fy  [iluu], fyz [ilul], fyz [iluu] },
    fu [16] = { fval[iull], fval[iulu], fz  [iull], fz  [iulu],
                fval[iuul], fval[iuuu], fz  [iuul], fz  [iuuu],
                fy  [iull], fy  [iulu], fyz [iull], fyz [iulu],
                fy  [iuul], fy  [iuuu], fyz [iuul], fyz [iuuu] },
    fxl[16] = { fx  [illl], fx  [illu], fxz [illl], fxz [illu],
                fx  [ilul], fx  [iluu], fxz [ilul], fxz [iluu],
                fxy [illl], fxy [illu], fxyz[illl], fxyz[illu],
                fxy [ilul], fxy [iluu], fxyz[ilul], fxyz[iluu] },
    fxu[16] = { fx  [iull], fx  [iulu], fxz [iull], fxz [iulu],
                fx  [iuul], fx  [iuuu], fxz [iuul], fxz [iuuu],
                fxy [iull], fxy [iulu], fxyz[iull], fxyz[iulu],
                fxy [iuul], fxy [iuuu], fxyz[iuul], fxyz[iuuu] };
    double F[16];
    evalCubicSplines<16>(x, xlow, xupp, fl, fu, fxl, fxu, /*output*/ F, NULL, NULL);
    // 2nd stage: interpolate along y axis to obtain f(x,y,zlow), f(x,y,zupp), fz(x,y,zlow), fz(x,y,zupp)
    double FF[4];
    evalCubicSplines<4> (y, ylow, yupp, F+0,  F+4,  F+8,  F+12, /*output*/ FF, NULL, NULL);
    // 3rd stage: interpolate along z axis
    double val;
    evalCubicSplines<1> (z, zlow, zupp, FF+0, FF+1, FF+2, FF+3, /*output*/ &val, NULL, NULL);
    return val;
}


// ------ 3d B-spline interpolator ------ //

template<int N>
BsplineInterpolator3d<N>::BsplineInterpolator3d(
    const std::vector<double>& xgrid, const std::vector<double>& ygrid, const std::vector<double>& zgrid) :
    xnodes(xgrid), ynodes(ygrid), znodes(zgrid),
    numComp(indComp(xnodes.size()+N-2, ynodes.size()+N-2, znodes.size()+N-2)+1)
{
    if(xnodes.size()<2 || ynodes.size()<2 || znodes.size()<2)
        throw std::invalid_argument("BsplineInterpolator3d: number of nodes is too small");
    bool monotonic = true;
    for(unsigned int i=1; i<xnodes.size(); i++)
        monotonic &= xnodes[i-1] < xnodes[i];
    for(unsigned int i=1; i<ynodes.size(); i++)
        monotonic &= ynodes[i-1] < ynodes[i];
    for(unsigned int i=1; i<znodes.size(); i++)
        monotonic &= znodes[i-1] < znodes[i];
    if(!monotonic)
        throw std::invalid_argument("BsplineInterpolator3d: grid nodes must be sorted in ascending order");
}

template<int N>
void BsplineInterpolator3d<N>::nonzeroComponents(const double point[3],
    unsigned int leftIndices[3], double values[]) const
{
    double weights[3][N+1];
    for(int d=0; d<3; d++) {
        const std::vector<double>& nodes = d==0? xnodes : d==1? ynodes : znodes;
        leftIndices[d] = bsplineValues<N>(point[d], &nodes[0], nodes.size(), weights[d]);
    }
    for(int i=0; i<=N; i++)
        for(int j=0; j<=N; j++)
            for(int k=0; k<=N; k++)
                values[(i * (N+1) + j) * (N+1) + k] = weights[0][i] * weights[1][j] * weights[2][k];
}

template<int N>
double BsplineInterpolator3d<N>::interpolate(
    const double point[3], const std::vector<double> &amplitudes) const
{
    if(amplitudes.size() != numComp)
        throw std::length_error("BsplineInterpolator3d: invalid size of amplitudes array");
    double weights[(N+1)*(N+1)*(N+1)];
    unsigned int leftInd[3];
    nonzeroComponents(point, leftInd, weights);
    double val=0;
    for(int i=0; i<=N; i++)
        for(int j=0; j<=N; j++)
            for(int k=0; k<=N; k++)
                val += weights[ (i * (N+1) + j) * (N+1) + k ] *
                    amplitudes[ indComp(i+leftInd[0], j+leftInd[1], k+leftInd[2]) ];
    return val;
}

template<int N>
void BsplineInterpolator3d<N>::eval(const double point[3], double values[]) const
{
    unsigned int leftInd[3];
    double weights[(N+1)*(N+1)*(N+1)];
    nonzeroComponents(point, leftInd, weights);
    std::fill(values, values+numComp, 0.);
    for(int i=0; i<=N; i++)
        for(int j=0; j<=N; j++)
            for(int k=0; k<=N; k++)
                values[ indComp(i+leftInd[0], j+leftInd[1], k+leftInd[2]) ] =
                    weights[(i * (N+1) + j) * (N+1) + k];
}

template<int N>
double BsplineInterpolator3d<N>::valueOfComponent(const double point[3], unsigned int indComp) const
{
    if(indComp>=numComp)
        throw std::out_of_range("BsplineInterpolator3d: component index out of range");
    unsigned int leftInd[3], indices[3];
    double weights[(N+1)*(N+1)*(N+1)];
    nonzeroComponents(point, leftInd, weights);
    decomposeIndComp(indComp, indices);
    if( indices[0]>=leftInd[0] && indices[0]<=leftInd[0]+N &&
        indices[1]>=leftInd[1] && indices[1]<=leftInd[1]+N &&
        indices[2]>=leftInd[2] && indices[2]<=leftInd[2]+N )
        return weights[ ((indices[0]-leftInd[0]) * (N+1)
                         +indices[1]-leftInd[1]) * (N+1) + indices[2]-leftInd[2] ];
    else
        return 0;
}

template<int N>
void BsplineInterpolator3d<N>::nonzeroDomain(unsigned int indComp,
    double xlower[3], double xupper[3]) const
{
    if(indComp>=numComp)
        throw std::out_of_range("BsplineInterpolator3d: component index out of range");
    unsigned int indices[3];
    decomposeIndComp(indComp, indices);
    for(int d=0; d<3; d++) {
        const std::vector<double>& nodes = d==0? xnodes : d==1? ynodes : znodes;
        xlower[d] = nodes[ indices[d]<N ? 0 : indices[d]-N ];
        xupper[d] = nodes[ std::min<unsigned int>(indices[d]+1, nodes.size()-1) ];
    }
}

template<int N>
SparseMatrix<double> BsplineInterpolator3d<N>::computeRoughnessPenaltyMatrix() const
{
    std::vector<Triplet> values;      // elements of sparse matrix will be accumulated here
    Matrix<double>
    X0(computeOverlapMatrix<N,0>(xnodes)),  // matrices of products of 1d basis functions or derivs
    X1(computeOverlapMatrix<N,1>(xnodes)),
    X2(computeOverlapMatrix<N,2>(xnodes)),
    Y0(computeOverlapMatrix<N,0>(ynodes)),
    Y1(computeOverlapMatrix<N,1>(ynodes)),
    Y2(computeOverlapMatrix<N,2>(ynodes)),
    Z0(computeOverlapMatrix<N,0>(znodes)),
    Z1(computeOverlapMatrix<N,1>(znodes)),
    Z2(computeOverlapMatrix<N,2>(znodes));
    for(unsigned int index1=0; index1<numComp; index1++) {
        unsigned int ind[3];
        decomposeIndComp(index1, ind);
        // use the fact that in each dimension, the overlap matrix elements are zero if
        // |rowIndex-colIndex| > N (i.e. it is a band matrix with width 2N+1).
        unsigned int
        imin = ind[0]<N ? 0 : ind[0]-N,
        jmin = ind[1]<N ? 0 : ind[1]-N,
        kmin = ind[2]<N ? 0 : ind[2]-N,
        imax = std::min<unsigned int>(ind[0]+N+1, xnodes.size()+N-1),
        jmax = std::min<unsigned int>(ind[1]+N+1, ynodes.size()+N-1),
        kmax = std::min<unsigned int>(ind[2]+N+1, znodes.size()+N-1);
        for(unsigned int i=imin; i<imax; i++) {
            for(unsigned int j=jmin; j<jmax; j++) {
                for(unsigned int k=kmin; k<kmax; k++) {
                    unsigned int index2 = indComp(i, j, k);
                    if(index2>index1)
                        continue;  // will initialize from a symmetric element
                    double val =
                        X2(ind[0], i) * Y0(ind[1], j) * Z0(ind[2], k) +
                        X0(ind[0], i) * Y2(ind[1], j) * Z0(ind[2], k) +
                        X0(ind[0], i) * Y0(ind[1], j) * Z2(ind[2], k) +
                        X1(ind[0], i) * Y1(ind[1], j) * Z0(ind[2], k) * 2 +
                        X0(ind[0], i) * Y1(ind[1], j) * Z1(ind[2], k) * 2 +
                        X1(ind[0], i) * Y0(ind[1], j) * Z1(ind[2], k) * 2;
                    values.push_back(Triplet(index1, index2, val));
                    if(index1!=index2)
                        values.push_back(Triplet(index2, index1, val));
                }
            }
        }
    }
    return SparseMatrix<double>(numComp, numComp, values);
}

template<int N>
std::vector<double> createBsplineInterpolator3dArray(const IFunctionNdim& F,
    const std::vector<double>& xnodes,
    const std::vector<double>& ynodes,
    const std::vector<double>& znodes)
{
    if(F.numVars() != 3 || F.numValues() != 1)
        throw std::invalid_argument(
            "createBsplineInterpolator3dArray: input function must have numVars=3, numValues=1");
    BsplineInterpolator3d<N> interp(xnodes, ynodes, znodes);

    // collect the function values at all nodes of 3d grid
    std::vector<double> fncvalues(interp.numValues());
    double point[3];
    for(unsigned int i=0; i<xnodes.size(); i++) {
        point[0] = xnodes[i];
        for(unsigned int j=0; j<ynodes.size(); j++) {
            point[1] = ynodes[j];
            for(unsigned int k=0; k<znodes.size(); k++) {
                point[2] = znodes[k];
                unsigned int index = interp.indComp(i, j, k);
                F.eval(point, &fncvalues[index]);
            }
        }
    }
    if(N==1)
        // in this case no further action is necessary: the values of function at grid nodes
        // are identical to the amplitudes used in the interpolation
        return fncvalues;

    // the matrix of values of basis functions at grid nodes (could be *BIG*, although it is sparse)
    std::vector<Triplet> values;  // elements of sparse matrix will be accumulated here
    const std::vector<double>* nodes[3] = {&xnodes, &ynodes, &znodes};
    // values of 1d B-splines at each grid node in each of the three dimensions, or -
    // for the last two rows in each matrix - 2nd derivatives of B-splines at the first/last grid nodes
    Matrix<double> weights[3];
    // indices of first non-trivial B-spline functions at each grid node in each dimension
    std::vector<int> leftInd[3];

    // collect the values of all basis functions at each grid node in each dimension
    for(int d=0; d<3; d++) {
        unsigned int Ngrid = nodes[d]->size();
        weights[d] = Matrix<double>(Ngrid+N-1, N+1);
        leftInd[d].resize(Ngrid+N-1);
        const double* arr = &(nodes[d]->front());
        for(unsigned int n=0; n<Ngrid; n++)
            leftInd[d][n] = bsplineValues<N>(arr[n], arr, Ngrid, &weights[d](n, 0));
        // collect 2nd derivatives at the endpoints
        leftInd[d][Ngrid]   = bsplineDerivs<N,2>(arr[0],       arr, Ngrid, &weights[d](Ngrid,   0));
        leftInd[d][Ngrid+1] = bsplineDerivs<N,2>(arr[Ngrid-1], arr, Ngrid, &weights[d](Ngrid+1, 0));
    }
    // each row of the matrix corresponds to the value of source function at a given grid point,
    // or to its the second derivative at the endpoints of grid which is assumed to be zero
    // (i.e. natural cubic spline boundary condition);
    // each column corresponds to the weights of each element of amplitudes array,
    // which is formed as a product of non-zero 1d basis functions in three dimensions,
    // or their 2nd derivs at extra endpoint nodes
    for(unsigned int i=0; i<xnodes.size()+N-1; i++) {
        for(unsigned int j=0; j<ynodes.size()+N-1; j++) {
            for(unsigned int k=0; k<znodes.size()+N-1; k++) {
                unsigned int indRow = interp.indComp(i, j, k);
                for(int ti=0; ti<=N; ti++) {
                    for(int tj=0; tj<=N; tj++) {
                        for(int tk=0; tk<=N; tk++) {
                            unsigned int indCol = interp.indComp(
                                ti + leftInd[0][i], tj + leftInd[1][j], tk + leftInd[2][k]);
                            values.push_back(Triplet(indRow, indCol,
                                weights[0](i, ti) * weights[1](j, tj) * weights[2](k, tk)));
                        }
                    }
                }
            }
        }
    }

    // solve the linear system (could take *LONG* )
    return LUDecomp(SparseMatrix<double>(interp.numValues(), interp.numValues(), values)).solve(fncvalues);
}

template<int N>
std::vector<double> createBsplineInterpolator3dArrayFromSamples(
    const Matrix<double>& points, const std::vector<double>& pointWeights,
    const std::vector<double>& /*xnodes*/,
    const std::vector<double>& /*ynodes*/,
    const std::vector<double>& /*znodes*/)
{
    if(points.rows() != pointWeights.size() || points.cols() != 3)
        throw std::length_error(
            "createBsplineInterpolator3dArrayFromSamples: invalid size of input arrays");
    throw std::runtime_error("createBsplineInterpolator3dArrayFromSamples NOT IMPLEMENTED");
}

// force the template instantiations to compile
template class BsplineInterpolator3d<1>;
template class BsplineInterpolator3d<3>;

template std::vector<double> createBsplineInterpolator3dArray<1>(const IFunctionNdim& F,
    const std::vector<double>& xnodes,
    const std::vector<double>& ynodes,
    const std::vector<double>& znodes);
template std::vector<double> createBsplineInterpolator3dArray<3>(const IFunctionNdim& F,
    const std::vector<double>& xnodes,
    const std::vector<double>& ynodes,
    const std::vector<double>& znodes);
template std::vector<double> createBsplineInterpolator3dArrayFromSamples<1>(
    const Matrix<double>& points, const std::vector<double>& pointWeights,
    const std::vector<double>& xnodes,
    const std::vector<double>& ynodes,
    const std::vector<double>& znodes);
template std::vector<double> createBsplineInterpolator3dArrayFromSamples<3>(
    const Matrix<double>& points, const std::vector<double>& pointWeights,
    const std::vector<double>& xnodes,
    const std::vector<double>& ynodes,
    const std::vector<double>& znodes);


//-------------- PENALIZED SPLINE APPROXIMATION ---------------//

/// Implementation of penalized spline approximation
class SplineApproxImpl {
public:
    /// number of X[k] knots in the fitting spline;
    /// the number of basis functions is  numBasisFnc = numKnots+2
    const unsigned int numKnots;

    /// number of x[i],y[i] pairs (original data)
    const unsigned int numDataPoints;

    /// sum of weights of all point weights, or their total number if weights not provided
    double sumWeights;

private:
    const std::vector<double> knots;   ///< b-spline knots  X[k], k=0..numKnots-1
    const std::vector<double> xvalues; ///< x[i], i=0..numDataPoints-1
    const std::vector<double> weights; ///< w[i], i=0..numDataPoints-1

    /// sparse matrix  B  containing the values of each basis function at each data point:
    /// (size: numDataPoints rows, numBasisFnc columns, with only 4 nonzero values in each row)
    SparseMatrixSpecial<4> BMatrix;

    /// an intermediate matrix  B^T W B  describes the system of normal equations (where W=diag(w)),
    /// and the lower triangular matrix L contains its Cholesky decomposition (size: numBasisFnc^2)
    Matrix<double> LMatrix;

    /// matrix "M" is the transformed version of roughness matrix R, which contains
    /// integrals of product of second derivatives of basis functions (size: numBasisFnc^2)
    Matrix<double> MMatrix;

    /// part of the decomposition of the matrix M (size: numBasisFnc)
    std::vector<double> singValues;

public:
    /// Auxiliary data used in the fitting process, pre-initialized for each set of data points `y`
    /// (these data cannot be members of the class, since they are not constant)
    struct FitData {
        std::vector<double> zRHS;  ///< B^T W y, r.h.s. of the system of normal equations
        std::vector<double> MTz;   ///< the product  M^T z
        double ynorm2;             ///< weighted norm of the vector y (= y^T W y )
    };

    /** Prepare internal tables for fitting the data points at the given set of x-coordinates
        and the given array of knots which determine the basis functions */
    SplineApproxImpl(
        const std::vector<double>& knots,
        const std::vector<double>& xvalues,
        const std::vector<double>& weights);

    /** find the amplitudes of basis functions that provide the best fit to the data points `y`
        for the given value of smoothing parameter `lambda`, determined indirectly by EDF.
        \param[in]  yvalues  are the data values corresponding to x-coordinates
        that were provided to the constructor;
        \param[in]  EDF  is the equivalent number of degrees of freedom (2<=EDF<=numBasisFnc);
        \param[out] ampl  will contain the computed amplitudes of basis functions;
        \param[out] RSS  will contain the residual sum of squared differences between data and appxox;
    */
    void solveForAmplitudesWithEDF(const std::vector<double>& yvalues, double EDF,
        std::vector<double>& ampl, double& RSS) const;

    /** find the amplitudes of basis functions that provide the best fit to the data points `y`
        with the Akaike information criterion (AIC) being offset by deltaAIC from its minimum value
        (the latter corresponding to the case of optimal smoothing).
        \param[in]  yvalues  are the data values;
        \param[in]  deltaAIC is the offset of AIC (0 means the optimally smoothed spline);
        \param[out] ampl  will contain the computed amplitudes of basis functions;
        \param[out] RSS,EDF  same as in the previous function;
    */
    void solveForAmplitudesWithAIC(const std::vector<double>& yvalues, double deltaAIC,
        std::vector<double>& ampl, double& RSS, double& EDF) const;

    /** Obtain the best-fit solution for the given value of smoothing parameter lambda
        (this method is called repeatedly in the process of finding the optimal value of lambda).
        \param[in]  fitData contains the pre-initialized auxiliary arrays constructed by `initFit()`;
        \param[in]  lambda is the smoothing parameter;
        \param[out] ampl  will contain the computed amplitudes of basis functions;
        \param[out] RSS,EDF  same as in the previous function;
    */
    void computeAmplitudes(const FitData& fitData, double lambda,
        std::vector<double>& ampl, double& RSS, double& EDF) const;

private:
    /** Initialize temporary arrays used in the fitting process for the provided data vector y,
        in the case that the normal equations are not singular.
        \param[in]  yvalues is the vector of data values `y` at each data point;
        \returns    the data structure used by other methods later in the fitting process
    */
    FitData initFit(const std::vector<double>& yvalues) const;
};

SplineApproxImpl::SplineApproxImpl(const std::vector<double> &_knots,
    const std::vector<double> &_xvalues, const std::vector<double> &_weights)
:
    numKnots(_knots.size()),
    numDataPoints(_xvalues.size()),
    knots(_knots),
    xvalues(_xvalues),
    weights(_weights),
    BMatrix(numDataPoints, numKnots+2)
{
    if(numKnots <= 1 || numDataPoints < 4)
        throw std::invalid_argument("SplineApprox: incorrect size of the problem");

    if(weights.empty()) {
        sumWeights = numDataPoints;
    } else {
        if(weights.size()!=numDataPoints)
            throw std::length_error("SplineApprox: xvalues and weights must have equal length");
        sumWeights = 0;
        for(unsigned int i=0; i<numDataPoints; i++) {
            if(weights[i] < 0)
                throw std::invalid_argument("SplineApprox: weights must be non-negative");
            sumWeights += weights[i];
        }
        if(sumWeights == 0)
            throw std::invalid_argument("SplineApprox: sum of all weights must positive");
    }

    for(unsigned int k=1; k<numKnots; k++)
        if(!(knots[k] > knots[k-1]))
            throw std::invalid_argument("SplineApprox: knots must be in ascending order");

    // initialize b-spline matrix B
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for(int i=0; i<(int)numDataPoints; i++) {
        // for each input point, at most 4 basis functions are non-zero, starting from index 'ind'
        double Bspl[4];
        unsigned int ind = bsplineValuesExtrapolated<3>(xvalues[i], &knots.front(), numKnots, Bspl);
        assert(ind<=numKnots-2);
        BMatrix.assignRow(i, ind, Bspl);
    }

    // compute the symmetric matrix  C = B^T diag(w) B
    Matrix<double> CMatrix = BMatrix.multiplyByTransposed(weights);

    // compute the roughness matrix R (integrals over products of second derivatives of basis functions)
    Matrix<double> RMatrix(computeOverlapMatrix<3,2>(knots));

    // to prevent a failure of Cholesky decomposition in the case if C is not positive definite,
    // we add a small multiple of R to C (following the recommendation in Ruppert,Wand&Carroll)
    blas_daxpy(1e-10 * sqrt(blas_dnrm2(CMatrix) / blas_dnrm2(RMatrix)), RMatrix, CMatrix);

    // pre-compute matrix L which is the Cholesky decomposition of matrix of normal equations C
    CholeskyDecomp CholC(CMatrix);
    LMatrix = CholC.L();

    // transform the roughness matrix R into a more suitable form M+singValues:
    // obtain Q = L^{-1} R L^{-T}, where R is the roughness penalty matrix (replace R by Q)
    blas_dtrsm(CblasLeft,  CblasLower, CblasNoTrans, CblasNonUnit, 1, LMatrix, RMatrix);
    blas_dtrsm(CblasRight, CblasLower,   CblasTrans, CblasNonUnit, 1, LMatrix, RMatrix);

    // decompose this Q via singular value decomposition: Q = U * diag(S) * V^T
    // (since Q is symmetric, U and V should be identical)
    SVDecomp SVD(RMatrix);
    singValues = SVD.S();       // vector of singular values of matrix Q:
    singValues[numKnots] = 0;   // the smallest two singular values must be zero;
    singValues[numKnots+1] = 0; // set it explicitly to avoid roundoff error

    // precompute M = L^{-T} U  which is used in computing amplitudes of basis functions.
    MMatrix = SVD.U();
    blas_dtrsm(CblasLeft, CblasLower, CblasTrans, CblasNonUnit, 1, LMatrix, MMatrix);
    // now M is finally in place, and the amplitudes for any lambda are given by
    // M (I + lambda * diag(singValues))^{-1} M^T  z
}

// initialize the temporary arrays used in the fitting process for the given vector of values 'y'
SplineApproxImpl::FitData SplineApproxImpl::initFit(const std::vector<double> &yvalues) const
{
    if(yvalues.size() != numDataPoints)
        throw std::length_error("SplineApprox: input array sizes do not match");
    FitData fitData;
    fitData.ynorm2 = 0;
    if(weights.empty())
        for(unsigned int i=0; i<numDataPoints; i++)
            fitData.ynorm2 += pow_2(yvalues[i]);
    else
        for(unsigned int i=0; i<numDataPoints; i++)
            fitData.ynorm2 += weights[i] * pow_2(yvalues[i]);
    fitData.zRHS = BMatrix.multiplyByVector(yvalues, weights);   // precompute z = B^T diag(w) y
    fitData.MTz.resize(numKnots+2);
    blas_dgemv(CblasTrans, 1, MMatrix, fitData.zRHS, 0, fitData.MTz); // precompute M^T z
    return fitData;
}

namespace{  // a few helper routines and classes

// compute the number of equivalent degrees of freedom
inline double computeEDF(const std::vector<double>& singValues, double lambda)
{
    if(!isFinite(lambda))  // infinite smoothing leads to a straight line (2 d.o.f)
        return 2;
    else if(lambda==0)     // no smoothing means the number of d.o.f. equal to the number of basis fncs
        return singValues.size()*1.;
    else {
        double EDF = 0;
        for(unsigned int c=0; c<singValues.size(); c++)
            EDF += 1 / (1 + lambda * singValues[c]);
        return EDF;
    }
}

// compute the (modified) Akaike information criterion
inline double computeAIC(double RSS, double EDF, unsigned int numDataPoints)
{
    return log(RSS) + 2 * (EDF+1) / (numDataPoints-EDF-2);
}

// the root-finders below work with a scaledLambda in the interval [0:1],
// which is converted to the real lambda (smoothing parameter) in the range [0:infinity] by this routine
inline double unscaleLambda(double scaledLambda)
{
    return exp( 1 / scaledLambda - 1 / (1-scaledLambda) );
}

// helper class to find the value of scaledLambda that corresponds to the given number of
// equivalent degrees of freedom (EDF)
class SplineEDFRootFinder: public IFunctionNoDeriv {
    const std::vector<double>& singValues;
    double targetEDF;
public:
    SplineEDFRootFinder(const std::vector<double>& _singValues, double _targetEDF) :
        singValues(_singValues), targetEDF(_targetEDF) {}
    virtual double value(double scaledLambda) const {
        return computeEDF(singValues, unscaleLambda(scaledLambda)) - targetEDF;
    }
};

// helper class to find the value of scaledLambda that corresponds to the given value of AIC
class SplineAICRootFinder: public IFunctionNoDeriv {
    const SplineApproxImpl& impl; ///< the fitting interface
    const SplineApproxImpl::FitData& fitData; ///< data for the fitting procedure
    const double targetAIC;       ///< target value of AIC for root-finder
public:
    SplineAICRootFinder(const SplineApproxImpl& _impl,
        const SplineApproxImpl::FitData& _fitData, double _targetAIC) :
        impl(_impl), fitData(_fitData), targetAIC(_targetAIC) {};
    virtual double value(double scaledLambda) const {
        std::vector<double> ampl;
        double RSS, EDF;
        impl.computeAmplitudes(fitData, unscaleLambda(scaledLambda), ampl, RSS, EDF);
        return computeAIC(RSS, EDF, impl.numDataPoints) - targetAIC;
    }
};
}  // internal namespace

// obtain solution of linear system for the given smoothing parameter,
// using the pre-computed matrix M^T z, where z = B^T W y is the rhs of the system of normal equations;
// output the amplitudes of basis functions and other relevant quantities (RSS, EDF);
void SplineApproxImpl::computeAmplitudes(const FitData &fitData, double lambda,
    std::vector<double> &ampl, double &RSS, double &EDF) const
{
    std::vector<double> tempv(numKnots+2);
    for(unsigned int p=0; p<numKnots+2; p++) {
        double sv = singValues[p];
        tempv[p]  = fitData.MTz[p] / (1 + (sv>0 ? sv*lambda : 0));
    }
    ampl.resize(numKnots+2);
    blas_dgemv(CblasNoTrans, 1, MMatrix, tempv, 0, ampl);
    // compute the residual sum of squares (note: may be prone to cancellation errors?)
    tempv = ampl;
    blas_dtrmv(CblasLower, CblasTrans, CblasNonUnit, LMatrix, tempv); // tempv = L^T w
    double wTz = blas_ddot(ampl, fitData.zRHS);
    RSS = (fitData.ynorm2 - 2*wTz + blas_ddot(tempv, tempv));
    EDF = computeEDF(singValues, lambda);  // equivalent degrees of freedom
    /*utils::msg(utils::VL_VERBOSE, "SplineApprox",
        "lambda="+utils::toString(lambda,10)+
        ", RSS="+utils::toString(RSS,10)+
        ", EDF="+utils::toString(EDF,10)+
        ", AIC="+utils::toString(computeAIC(RSS, EDF, numDataPoints),10)+
        ", GCV="+utils::toString(RSS / pow_2(numDataPoints-EDF),10));*/
}

void SplineApproxImpl::solveForAmplitudesWithEDF(const std::vector<double> &yvalues, double EDF,
    std::vector<double> &ampl, double &RSS) const
{
    if(EDF==0)
        EDF = numKnots+2;
    else if(EDF<2 || EDF>numKnots+2)
        throw std::invalid_argument("SplineApprox: incorrect number of equivalent degrees of freedom");
    double lambda = unscaleLambda(findRoot(SplineEDFRootFinder(singValues, EDF), 0, 1, 1e-6));
    computeAmplitudes(initFit(yvalues), lambda, ampl, RSS, EDF);
}

void SplineApproxImpl::solveForAmplitudesWithAIC(const std::vector<double> &yvalues, double deltaAIC,
    std::vector<double> &ampl, double &RSS, double &EDF) const
{
    double lambda=0;
    FitData fitData = initFit(yvalues);
    if(deltaAIC < 0)
        throw std::invalid_argument("SplineApprox: deltaAIC must be non-negative");
    if(deltaAIC == 0) {  // find the value of lambda corresponding to the optimal fit
        lambda = unscaleLambda(findMin(SplineAICRootFinder(*this, fitData, 0),
            0, 1, NAN /*no initial guess*/, 1e-6));
        if(lambda!=lambda)
            lambda = 0;  // no smoothing in case of weird problems
    } else {  // find an oversmoothed solution
        // the reference value of AIC at lambda=0 (NOT the value that minimizes AIC, but very close to it)
        computeAmplitudes(fitData, 0, ampl, RSS, EDF);
        double AIC0 = computeAIC(RSS, EDF, numDataPoints);
        // find the value of lambda so that AIC is larger than the reference value by the required amount
        lambda = unscaleLambda(findRoot(SplineAICRootFinder(*this, fitData, AIC0 + deltaAIC),
            0, 1, 1e-6));
        if(!isFinite(lambda))   // root does not exist, i.e. AIC is everywhere lower than target value
            lambda = INFINITY;  // basically means fitting with a linear regression
    }
    // compute the amplitudes for the final value of lambda
    computeAmplitudes(fitData, lambda, ampl, RSS, EDF);
}

//----------- DRIVER CLASS FOR PENALIZED SPLINE APPROXIMATION ------------//

SplineApprox::SplineApprox(const std::vector<double> &grid,
    const std::vector<double> &xvalues, const std::vector<double> &weights)
{
    impl = new SplineApproxImpl(grid, xvalues, weights);
}

SplineApprox::~SplineApprox()
{
    delete impl;
}

std::vector<double> SplineApprox::fit(
    const std::vector<double> &yvalues, const double edf,
    double *rms) const
{
    std::vector<double> ampl;
    double RSS;
    impl->solveForAmplitudesWithEDF(yvalues, edf, ampl, RSS);
    if(rms)
        *rms = sqrt(RSS / impl->sumWeights);
    return ampl;
}

std::vector<double> SplineApprox::fitOversmooth(
    const std::vector<double> &yvalues, const double deltaAIC,
    double *rms, double* edf) const
{
    std::vector<double> ampl;
    double RSS, EDF;
    impl->solveForAmplitudesWithAIC(yvalues, deltaAIC, ampl, RSS, EDF);
    if(rms)
        *rms = sqrt(RSS / impl->sumWeights);
    if(edf)
        *edf = EDF;
    return ampl;
}


//------------ LOG-SPLINE DENSITY ESTIMATOR ------------//
namespace {

/** Data for SplineLogDensity fitting procedure that is changing during the fit */
struct SplineLogFitParams {
    std::vector<double> ampl; ///< array of amplitudes used to start the multidimensional minimizer
    double lambda;            ///< smoothing parameter
    double targetLogL;        ///< target value of likelihood for the case with smoothing
    double best;              ///< highest cross-validation score or smallest offset from root
    double gradNorm;          ///< normalization factor for determining the root-finder tolerance
    SplineLogFitParams() : lambda(0), targetLogL(0), best(-INFINITY), gradNorm(0) {}
};

/** The engine of log-spline density estimator relies on the maximization of log-likelihood
    of input samples by varying the parameters of the estimator.

    Let  x_i, w_i; i=0..N_{data}-1  be the coordinates and weights of samples drawn from
    an unknown density distribution that we wish to estimate by constructing a function P(x).
    The total weight of all samples is  \f$  M = \sum_{i=0}^{N_{data}-1} w_i  \f$
    (does not need to be unity), and we stipulate that  \f$  \int P(x) dx = M  \f$.

    The logarithm of estimated density P(x) is represented as
    \f[
    \ln P(x) = \sum_{k=0}^{N_{basis}-1}  A_k B_k(x) - \ln G_0 + \ln M = Q(x) - \ln G_0 + \ln M,
    \f]
    where  A_k  are the amplitudes -- free parameters that are adjusted during the fit,
    B_k(x)  are basis functions (B-splines of degree N defined by grid nodes),
    \f$  Q(x) = \sum_k  A_k B_k(x)  \f$  is the weighted sum of basis function, and
    \f$  G_0  = \int \exp[Q(x)] dx  \f$  is the normalization constant determined from the condition
    that the integral of P(x) over the entire domain equals to M.
    If we shift the entire weighted sum of basis functions Q(x) up or down by a constant,
    this will have no effect on P(x), because this shift will be counterbalanced by G_0.
    Therefore, there is an extra gauge freedom of choosing {A_k};
    we elimitate it by fixing the amplitude of the last B-spline to zero: A_{N_{basis}-1} = 0.
    In the end, there are N_{ampl} = N_{basis}-1 free parameters that are adjusted during the fit.

    The total likelihood of the model given the amplitudes {A_k} is
    \f[
    \ln L = \sum_{i=0}^{N_{data}-1}  w_i  \ln P(x_i)
          = \sum_{i=0}^{N_{data}-1}  w_i (\sum_{k=0}^{N_{ampl}-1} A_k B_k(x_i) - \ln G_0 + \ln M)
          = \sum_{k=0}^{N_{ampl}-1}  A_k  L_k  - M \ln G_0({A_k})  + M \ln M,
    \f]
    where  \f$  L_k = \sum_{i=0}^{N_{data}-1} w_i B_k(x_i)  \f$  is an array of 'basis likelihoods'
    that is computed from input samples only once at the beginning of the fitting procedure.

    Additionally, we may impose a penalty for unsmoothness of the estimated P(x), by adding a term
    \f$  -\lambda \int (\ln P(x)'')^2 dx  \f$  into the above expression.
    Here lambda>=0 is the smoothing parameter, and the integral of squared second derivative
    of the sum of B-splines is expressed as a quadratic form in amplitudes:
    \f$  \sum_k \sum_l  A_k A_l R_{kl}  \f$,  where  \f$  R_{kl} = \int B_k''(x) B_l''(x) dx  \f$
    is the 'roughhness matrix', again pre-computed at the beginning of the fitting procedure.
    This addition decreases the overall likelihood, but makes the estimated ln P(x) more smooth.
    To find the suitable value of lambda, we use the following consideration:
    if P(x) were the true density distribution, then the likelihood of the finite number of samples
    is a random quantity with mean E and rms scatter D.
    We can tolerate the decrease in the likelihood by an amount comparable to D,
    if this makes the estimate more smooth.
    Therefore, we first find the values of amplitudes that maximize the likelihood without smoothing,
    and then determine the value of lambda that decreases log L by the prescribed amount.
    The mean and rms scatter of ln L are given (for equal-weight samples) by
    \f[
    E   = \int P(x) \ln P(x) dx
        = M (G_1 + \ln M - \ln G_0),
    D   = \sqrt{ M \int P(x) [\ln P(x)]^2 dx  -  E^2 } / \sqrt{N_{data}}
        = M \sqrt{ G_2/G_0 - (G_1/G_0)^2 } / \sqrt{N_{data}} , \f]
    where we defined  \f$
    G_d = \int \exp[Q(x)] [Q(x)]^d dx  \f$  for d=0,1,2.

    We minimize  -log(L)  by varying the amplitudes A_k (for a fixed value of lambda),
    using a nonlinear multidimensional root-finder with derivatives to locate the point
    where d log(L) / d A_k = 0 for all A_k.
    This class implements the interface needed for `findRootNdimDeriv()`: computation of
    gradient and hessian of log(L) w.r.t. each of the free parameters A_k, k=0..N_{ampl}-1.
*/
template<int N>
class SplineLogDensityFitter: public IFunctionNdimDeriv {
public:
    SplineLogDensityFitter(
        const std::vector<double>& xvalues, const std::vector<double>& weights,
        const std::vector<double>& grid, FitOptions options,
        SplineLogFitParams& params);

    /** Return the array of properly normalized amplitudes, such that the integral of
        P(x) over the entire domain is equal to the sum of sample weights M.
    */
    std::vector<double> getNormalizedAmplitudes(const std::vector<double>& ampl) const;

    /** Compute the expected rms scatter in log-likelihood for a density function defined
        by the given amplitudes, for the given number of samples */
    double logLrms(const std::vector<double>& ampl) const;

    /** Compute the log-likelihood of the data given the amplitudes.
        \param[in]  ampl  is the array of amplitudes A_k, k=0..numAmpl-1.
        \return     \ln L = \sum_k A_k V_k - M \ln G_0({A_k}) + M \ln M ,  where
        G_0 is the normalization constant that depends on all A_k,
        V_k is the pre-computed array Vbasis.
    */
    double logL(const std::vector<double>& ampl) const;

    /** Compute the cross-validation likelihood of the data given the amplitudes.
        \param[in]  ampl  is the array of amplitudes.
        \return     \ln L_CV = \ln L - tr(H^{-1} B^T B) + (d \ln G_0 / d A) H^{-1} W.
    */
    double logLcv(const std::vector<double>& ampl) const;

private:
    /** Compute the gradient and hessian of the full log-likelihood function
        (including the roughness penalty) multiplied by a constant:
        \ln L_full = \ln L - \lambda \sum_k \sum_l A_k A_l R_{kl},
        where \ln L is defined in logL(),  R_{kl} is the pre-computed roughnessMatrix,
        and lambda is taken from an external SplineLogFitParams variable.
        This routine is used in the nonlinear root-finder to determine the values of A_k
        that correspond to grad=0.
        \param[in]  ampl  is the array of amplitudes A_k that are varied during the fit;
        \param[out] grad  = (-1/M) d \ln L / d A_k;
        \param[out] hess  = (-1/M) d^2 \ln L / d A_k d A_l.
    */
    virtual void evalDeriv(const double ampl[], double grad[], double hess[]) const;
    virtual unsigned int numVars() const { return numAmpl; }
    virtual unsigned int numValues() const { return numAmpl; }

    /** Compute the values and derivatives of  G_d = \int \exp(Q(x)) [Q(x)]^d  dx,  where
        Q(x) = \sum_{k=0}^{N_{ampl}-1}  A_k B_k(x)  is the weighted sum of basis functions,
        B_k(x) are basis functions (B-splines of degree N defined by the grid nodes),
        and the integral is taken over the finite or (semi-) infinite interval,
        depending on the boolean constants leftInfinite, rightInfinite
        (if any of them is false, the corresponding boundary is the left/right-most grid point,
        otherwise it is +-infinity).
        \param[in]  ampl  is the array of A_k.
        \param[out] deriv if not NULL, will contain the derivatives of  \ln(G_0) w.r.t. A_k;
        \param[out] deriv2 if not NULL, will contain the second derivatives:
        d^2 \ln G_0 / d A_k d A_l.
        \param[out] GdG0  if not NULL, will contain  G_1/G_0  and  G_2/G_0.
        \return     \ln G_0.
    */
    double logG(const double ampl[], double deriv[]=NULL, double deriv2[]=NULL, double GdG0[]=NULL) const;

    const std::vector<double> grid;   ///< grid nodes that define the B-splines
    const unsigned int numNodes;      ///< shortcut for grid.size()
    const unsigned int numBasisFnc;   ///< shortcut for the number of B-splines (numNodes+N-1)
    const unsigned int numAmpl;       ///< the number of amplitudes that may be varied (numBasisFnc-1)
    const unsigned int numData;       ///< number of sample points
    const FitOptions options;         ///< whether the definition interval extends to +-inf
    static const int GL_ORDER = 8;    ///< order of GL quadrature for computing the normalization
    double GLnodes[GL_ORDER], GLweights[GL_ORDER];  ///< nodes and weights of GL quadrature
    std::vector<double> Vbasis;       ///< basis likelihoods: V_k = \sum_i w_i B_k(x_i)
    std::vector<double> Wbasis;       ///< W_k = \sum_i w_i^2 B_k(x_i)
    Matrix<double> BTBmatrix;         ///< matrix C = B^T B, where B_{ik} = w_i B_k(x_i)
    Matrix<double> roughnessMatrix;   ///< roughness penalty matrix - integrals of B_k''(x) B_l''(x)
    SplineLogFitParams& params;       ///< external parameters that may be changed during the fit
    double sumWeights;                ///< normalized sum of weights of input points (M)
    double logSumWeights;             ///< logarithm of the original (un-normalized) sum of input weights
};

template<int N>
SplineLogDensityFitter<N>::SplineLogDensityFitter(
    const std::vector<double>& _grid,
    const std::vector<double>& xvalues,
    const std::vector<double>& weights,
    FitOptions _options,
    SplineLogFitParams& _params) :
    grid(_grid),
    numNodes(grid.size()),
    numBasisFnc(numNodes + N - 1),
    numAmpl(numBasisFnc - 1),
    numData(xvalues.size()),
    options(_options),
    params(_params),
    sumWeights(0),
    logSumWeights(0)
{
    if(numData <= 0)
        throw std::length_error("splineLogDensity: no data");
    if(numData != weights.size())
        throw std::length_error("splineLogDensity: sizes of input arrays are not equal");
    if(numNodes<2)
        throw std::invalid_argument("splineLogDensity: grid size should be at least 2");
    for(unsigned int k=1; k<numNodes; k++)
        if(grid[k-1] >= grid[k])
            throw std::invalid_argument("splineLogDensity: grid nodes are not monotonic");
    prepareIntegrationTableGL(0, 1, GL_ORDER, GLnodes, GLweights);

    // prepare the roughness penalty matrix
    // (integrals over products of certain derivatives of basis functions)
    if((options & FO_PENALTY_3RD_DERIV) == FO_PENALTY_3RD_DERIV)
        roughnessMatrix = computeOverlapMatrix<N,3>(grid);
    else
        roughnessMatrix = computeOverlapMatrix<N,2>(grid);

    // quick scan to analyze the weights
    double minWeight = INFINITY;
    double xmin = grid[0], xmax = grid[numNodes-1];
    double avgx = 0, avgx2 = 0;
    for(unsigned int p=0; p<numData; p++) {
        double xval = xvalues[p], weight = weights[p];
        if(weight < 0)
            throw std::invalid_argument("splineLogDensity: sample weights may not be negative");
        // if the interval is (semi-)finite, samples beyond its boundaries are ignored
        if( (xval < xmin && (options & FO_INFINITE_LEFT)  != FO_INFINITE_LEFT)  ||
            (xval > xmax && (options & FO_INFINITE_RIGHT) != FO_INFINITE_RIGHT) ||
            weight <= 0)
            continue;
        sumWeights += weight;
        avgx       += weight * xval;
        avgx2      += weight * pow_2(xval);
        minWeight   = std::min(minWeight, weight);
    }

    // sanity check
    if(sumWeights==0)
        throw std::invalid_argument("splineLogDensity: sum of sample weights should be positive");

    // compute the mean and dispersion of input samples
    avgx /= sumWeights;
    avgx2/= sumWeights;
    double dispx = fmax(avgx2 - pow_2(avgx), 0.01 * pow_2(xmax-xmin));
    avgx  = fmin(fmax(avgx, xmin), xmax);

    // prepare the log-likelihoods of each basis fnc and other useful arrays
    Vbasis.assign(numBasisFnc, 0.);
    Wbasis.assign(numBasisFnc, 0.);
    SparseMatrixSpecial<N+1> Bmatrix(numData, numAmpl);

#ifdef _OPENMP
#pragma omp parallel
#endif
    {   // hand-made implementation of parallel reduction:
        // the vectors and scalar variables below are thread-local,
        // they are accumulated in a parallel loop, and afterwards combined together
        // in a critical section which is executed by each thread sequentially.
        // While it's possible to use OpenMP reduction clause for sums accumulated in a scalar variable,
        // it's not applicable to vectors nor to the min expression.
        std::vector<double> Vbasis_(numBasisFnc, 0.), Wbasis_(numBasisFnc, 0.);
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
        for(int p=0; p<(int)numData; p++) {
            double xval = xvalues[p], weight = weights[p] / sumWeights;
            // if the interval is (semi-)finite, samples beyond its boundaries are ignored
            if( (xval < xmin && (options & FO_INFINITE_LEFT)  != FO_INFINITE_LEFT)  ||
                (xval > xmax && (options & FO_INFINITE_RIGHT) != FO_INFINITE_RIGHT) ||
                weight <= 0)
                continue;
            double Bspl[N+2] = {0};
            int ind = bsplineValuesExtrapolated<N>(xval, &grid[0], numNodes, Bspl+1);
            for(int b=0; b<=N; b++) {
                Bspl[b+1] *= weight;
                Vbasis_[ind+b] += Bspl[b+1];
                Wbasis_[ind+b] += Bspl[b+1] * weight;
            }
            int off = ind+N >= (int)numAmpl ? 1 : 0;
            Bmatrix.assignRow(p, ind-off, Bspl+1-off);
        }
#ifdef _OPENMP
#pragma omp critical (SplineLogDensityReductionLoop)
#endif
        {
            for(unsigned int i=0; i<numBasisFnc; i++) {
                Vbasis[i] += Vbasis_[i];
                Wbasis[i] += Wbasis_[i];
            }
        }
    }

    // normalize the sum of weights to unity (this is necessary for numerical stability),
    // but remember the log of the original sum of weights that will be added to amplitudes on output
    minWeight    /= sumWeights;
    logSumWeights = log(sumWeights);
    sumWeights    = 1.;

    // sanity check: all of basis functions must have a contribution from sample points,
    // otherwise the problem is singular and the max-likelihood solution is unattainable
    bool isSingular = false;
    for(unsigned int k=0; k<numBasisFnc; k++) {
        isSingular |= Vbasis[k]==0;
        params.gradNorm = fmax(params.gradNorm, fabs(Vbasis[k]));
    }
    if(isSingular) {
        // add fake contributions to all basis functions that would have arisen from
        // a uniformly distributed minWeight over each grid segment
        minWeight *= 1. / (numNodes-1);
        for(unsigned int j=0; j<numNodes-1; j++) {
            for(int s=0; s<GL_ORDER; s++) {
                double x = grid[j] + GLnodes[s] * (grid[j+1]-grid[j]);
                double Bspl[N+1];
                int ind = bsplineValues<N>(x, &grid[0], numNodes, Bspl);
                for(unsigned int b=0; b<=N; b++)
                    Vbasis[b+ind] += minWeight * Bspl[b] * GLweights[s];
            }
            sumWeights += minWeight;
        }
    }
    // chop off the last basis function whose amplitude is not varied in the fitting procedure
    Vbasis.resize(numAmpl);
    Wbasis.resize(numAmpl);

    // construct the matrix C = B^T B that is used in cross-validation
    BTBmatrix = Bmatrix.multiplyByTransposed();

    // assign the initial guess for amplitudes using a Gaussian density distribution
    params.ampl.assign(numBasisFnc, 0);
    for(int k=0; k<(int)numBasisFnc; k++) {
        double xnode = grid[ std::min<int>(numNodes-1, std::max(0, k-N/2)) ];
        params.ampl[k] = -pow_2(xnode-avgx) / 2 / dispx;
    }
    // make sure that we start with a density that is declining when extrapolated
    if((options & FO_INFINITE_LEFT) == FO_INFINITE_LEFT)
        params.ampl[0] = fmin(params.ampl[0], params.ampl[1] - (grid[1]-grid[0]));
    if((options & FO_INFINITE_RIGHT) == FO_INFINITE_RIGHT)
        params.ampl[numBasisFnc-1] = fmin(params.ampl[numBasisFnc-1],
            params.ampl[numBasisFnc-2] - (grid[numNodes-1]-grid[numNodes-2]));

    // now shift all amplitudes by the value of the rightmost one, which is always kept equal to zero
    for(unsigned int k=0; k<numAmpl; k++)
        params.ampl[k] -= params.ampl.back();
    // and eliminate the last one, since it does not take part in the fitting process
    params.ampl.pop_back();
}

template<int N>
std::vector<double> SplineLogDensityFitter<N>::getNormalizedAmplitudes(
    const std::vector<double>& ampl) const
{
    assert(ampl.size() == numAmpl);
    std::vector<double> result(numBasisFnc);
    double C = logSumWeights - logG(&ampl[0]);
    for(unsigned int n=0; n<numBasisFnc; n++)
        result[n] = (n<numAmpl ? ampl[n] : 0) + C;
    return result;
}

template<int N>
double SplineLogDensityFitter<N>::logLrms(const std::vector<double>& ampl) const
{
    assert(ampl.size() == numAmpl);
    double GdG0[2];
    logG(&ampl[0], NULL, NULL, GdG0);
    double rms = sumWeights * sqrt((GdG0[1] - pow_2(GdG0[0])) / numData);
    if(utils::verbosityLevel >= utils::VL_VERBOSE) {
        double avg = sumWeights * (GdG0[0] + log(sumWeights) - logG(&ampl[0]));
        utils::msg(utils::VL_VERBOSE, "splineLogDensity",
            "Expected log L="+utils::toString(avg)+" +- "+utils::toString(rms));
    }
    return rms;
}

template<int N>
double SplineLogDensityFitter<N>::logL(const std::vector<double>& ampl) const
{
    assert(ampl.size() == numAmpl);
    double val = sumWeights * (log(sumWeights) - logG(&ampl[0]));
    for(unsigned int k=0; k<numAmpl; k++)
        val += Vbasis[k] * ampl[k];
    return val;
}

template<int N>
double SplineLogDensityFitter<N>::logLcv(const std::vector<double>& ampl) const
{
    assert(ampl.size() == numAmpl);
    std::vector<double> grad(numAmpl);
    Matrix<double> hess(numAmpl, numAmpl);
    double val = sumWeights * (log(sumWeights) - logG(&ampl[0], &grad[0], hess.data()));
    for(unsigned int k=0; k<numAmpl; k++) {
        val += Vbasis[k] * ampl[k];
        for(unsigned int l=0; l<numAmpl; l++) {
            hess(k, l) = sumWeights * hess(k, l) + 2 * params.lambda * roughnessMatrix(k, l);
        }
    }
    try{
        // construct a temporary matrix T = H^{-1} C, where C = B^T B was initialized in the constructor
        Matrix<double> T(BTBmatrix);
        // invert the matrix H by constructing its Cholesky decomposition H = L L^T
        CholeskyDecomp hessdec(hess);
        Matrix<double> hessL(hessdec.L());
        blas_dtrsm(CblasLeft, CblasLower, CblasNoTrans, CblasNonUnit, 1, hessL, T);  // now T = L^{-1} C
        blas_dtrsm(CblasLeft, CblasLower, CblasTrans,   CblasNonUnit, 1, hessL, T);  // T = L^{-T} L^{-1} C
        double add = 0;
        for(unsigned int k=0; k<numAmpl; k++) {  // subtract the trace of H^{-1} U
            add -= T(k, k);
        }
        std::vector<double> Hm1W = hessdec.solve(Wbasis);  // compute H^{-1} W
        add += blas_ddot(grad, Hm1W);                      // add dG/dA H^{-1} W
        // don't allow the cross-validation likelihood to be higher than log L itself
        val += fmin(add, 0);  // (this shouldn't occur under normal circumstances)
    }
    catch(std::exception&) {  // CholeskyDecomp may fail if the fit did not converge, i.e. gradient != 0
        utils::msg(utils::VL_WARNING, "splineLogDensity", "Hessian is not positive-definite");
        val -= 1e10;   // this will never be a good fit
    }
    return val;
}

template<int N>
void SplineLogDensityFitter<N>::evalDeriv(const double ampl[], double deriv[], double deriv2[]) const
{
    logG(ampl, deriv, deriv2);
    if(deriv!=NULL) {  // (-1/M)  d (log L) / d A_k
        for(unsigned int k=0; k<numAmpl; k++) {
            deriv[k] -= Vbasis[k] / sumWeights;
        }
    }
    // roughness penalty (taking into account symmetry and sparseness of the matrix)
    if(params.lambda!=0) {
        for(unsigned int k=0; k<numAmpl; k++) {
            for(unsigned int l=k; l<std::min(numAmpl, k+N+1); l++) {
                double v = 2 * roughnessMatrix(k, l) * params.lambda / sumWeights;
                if(deriv2!=NULL) {
                    deriv2[k * numAmpl + l] += v;
                    if(k!=l)
                        deriv2[l * numAmpl + k] += v;
                }
                if(deriv!=NULL) {
                    deriv[k] += v * ampl[l];
                    if(k!=l)
                        deriv[l] += v * ampl[k];
                }
            }
        }
    }
}

template<int N>
double SplineLogDensityFitter<N>::logG(
    const double ampl[], double deriv_arg[], double deriv2[], double GdG0[]) const
{
    std::vector<double> deriv_tmp;
    double* deriv = deriv_arg;
    if(deriv_arg==NULL && deriv2!=NULL) {  // need a temporary workspace for the gradient vector
        deriv_tmp.resize(numAmpl);
        deriv = &deriv_tmp.front();
    }
    // accumulator for the integral  G_d = \int \exp( Q(x) ) [Q(x)]^d  dx,
    // where  Q = \sum_k  A_k B_k(x),  and d ranges from 0 to 2
    double integral[3] = {0};
    // accumulator for d G_0 / d A_k
    if(deriv)
        std::fill(deriv, deriv+numAmpl, 0.);
    // accumulator for d^2 G_0 / d A_k d A_l
    if(deriv2)
        std::fill(deriv2, deriv2+pow_2(numAmpl), 0.);
    // determine the constant offset needed to keep the magnitude in a reasonable range
    double offset = 0;
    for(unsigned int k=0; k<numAmpl; k++)
        offset = fmax(offset, ampl[k]);

    // loop over grid segments...
    for(unsigned int k=0; k<numNodes-1; k++) {
        double segwidth = grid[k+1] - grid[k];
        // ...and over sub-nodes of Gauss-Legendre quadrature rule within each grid segment
        for(int s=0; s<GL_ORDER; s++) {
            double x = grid[k] + GLnodes[s] * segwidth;
            double Bspl[N+1];
            // obtain the values of all nontrivial basis function at this point,
            // and the index of the first of these functions.
            int ind = bsplineValues<N>(x, &grid[0], numNodes, Bspl);
            // sum the contributions to Q(x) from each basis function,
            // weighted with the provided amplitudes;
            // here we substitute zero in place of the last (numBasisFnc-1)'th amplitude.
            double Q = 0;
            for(unsigned int b=0; b<=N && b+ind<numAmpl; b++) {
                Q += Bspl[b] * ampl[b+ind];
            }
            // the contribution of this point to the integral is weighted according to the GL quadrature;
            // the value of integrand is exp(Q) * Q^d,
            // but to avoid possible overflows, we instead compute  exp(Q-offset) Q^d.
            double val = GLweights[s] * segwidth * exp(Q-offset);
            for(int d=0; d<=2; d++)
                integral[d] += val * pow(Q, d);
            // contribution of this point to the integral of derivatives is further multiplied
            // by the value of each basis function at this point.
            if(deriv) {
                for(unsigned int b=0; b<=N && b+ind<numAmpl; b++)
                    deriv[b+ind] += val * Bspl[b];
            }
            // and contribution to the integral of second derivatives is multiplied
            // by the product of two basis functions
            if(deriv2) {
                for(unsigned int b=0; b<=N && b+ind<numAmpl; b++)
                    for(unsigned int c=0; c<=N && c+ind<numAmpl; c++)
                        deriv2[(b+ind) * numAmpl + c+ind] += val * Bspl[b] * Bspl[c];
            }
        }
    }

    // if the interval is (semi-)infinite, need to add contributions from the tails beyond the grid
    bool   infinite[2] = {(options & FO_INFINITE_LEFT)  == FO_INFINITE_LEFT,
                          (options & FO_INFINITE_RIGHT) == FO_INFINITE_RIGHT};
    double endpoint[2] = {grid[0], grid[numNodes-1]};
    double signder [2] = {+1, -1};
    for(int p=0; p<2; p++) {
        if(!infinite[p])
            continue;
        double Bspl[N+1], Bder[N+1];
        int ind = bsplineValues<N>(endpoint[p], &grid[0], numNodes, Bspl);
        bsplineDerivs<N,1>(endpoint[p], &grid[0], numNodes, Bder);
        double Q = 0, Qder = 0;
        for(unsigned int b=0; b<=N && b+ind<numAmpl; b++) {
            Q    += Bspl[b] * ampl[b+ind];
            Qder += Bder[b] * ampl[b+ind];
        }
        if(signder[p] * Qder <= 0) {
            // the extrapolated function rises as x-> -inf, so the integral does not exist
            if(deriv)
                deriv[0] = INFINITY;
            if(deriv2)
                deriv2[0] = INFINITY;
            return INFINITY;
        }
        double val = signder[p] * exp(Q-offset) / Qder;
        integral[0] += val;
        integral[1] += val * (Q-1);
        integral[2] += val * (pow_2(Q-1)+1);
        if(deriv) {
            for(unsigned int b=0; b<=N && b+ind<numAmpl; b++)
                deriv[b+ind] += val * (Bspl[b] - Bder[b] / Qder);
        }
        if(deriv2) {
            for(unsigned int b=0; b<=N && b+ind<numAmpl; b++)
                for(unsigned int c=0; c<=N && c+ind<numAmpl; c++)
                    deriv2[(b+ind) * numAmpl + c+ind] +=
                        val * ( Bspl[b] * Bspl[c] -
                        (Bspl[b] * Bder[c] + Bspl[c] * Bder[b]) / Qder +
                        2 * Bder[b] * Bder[c] / pow_2(Qder) );
        }
    }

    // output the log-derivative: d (ln G_0) / d A_k = (d G_0 / d A_k) / G_0
    if(deriv) {
        for(unsigned int k=0; k<numAmpl; k++)
            deriv[k] /= integral[0];
    }
    // d^2 (ln G_0) / d A_k d A_l = d^2 G_0 / d A_k d A_l - (d ln G_0 / d A_k) (d ln G_0 / d A_l)
    if(deriv2) {
        for(unsigned int kl=0; kl<pow_2(numAmpl); kl++)
            deriv2[kl] = deriv2[kl] / integral[0] - deriv[kl / numAmpl] * deriv[kl % numAmpl];
    }
    // if necessary, return G_d/G_0, d=1,2
    if(GdG0) {
        GdG0[0] = integral[1] / integral[0];
        GdG0[1] = integral[2] / integral[0];
    }
    // put back the offset in the logarithm of the computed value of G_0
    return log(integral[0]) + offset;
}


/** Class for performing the search of the smoothing parameter lambda that meets some goal.
    There are two regimes:
    1) find the maximum of cross-validation score (used with one-dimensional findMin routine);
    2) search for lambda that yields the required value of log-likelihood (if params.targetLogL != 0).
    In either case, we find the best-fit amplitudes of basis functions for the current choice of lambda,
    and then if the fit converged and the goal is closer (i.e. the cross-validation score is higher
    or the difference between logL and targetLogL is smaller than any previous value),
    we also update the best-fit amplitudes in params.ampl, so that on the next iteration the search
    would start from a better initial point. This also improves the robustness of the entire procedure.
*/
template<int N>
class SplineLogDensityLambdaFinder: public IFunctionNoDeriv {
public:
    SplineLogDensityLambdaFinder(const SplineLogDensityFitter<N>& _fitter, SplineLogFitParams& _params) :
        fitter(_fitter), params(_params) {}
private:
    virtual double value(const double scaledLambda) const
    {
        bool useCV = params.targetLogL==0;   // whether we are in the minimizer or root-finder mode
        params.lambda = exp( 1 / scaledLambda - 1 / (1-scaledLambda) );
        std::vector<double> result(params.ampl);
        int numIter   = findRootNdimDeriv(fitter, &params.ampl[0],
            1e-8*params.gradNorm, 100, &result[0]);
        double logL   = fitter.logL(result);
        double logLcv = fitter.logLcv(result);
        bool converged= numIter>0;  // check for convergence (numIter positive)
        if(utils::verbosityLevel >= utils::VL_VERBOSE) {
            utils::msg(utils::VL_VERBOSE, "splineLogDensity",
                "lambda="+utils::toString(params.lambda)+", #iter="+utils::toString(numIter)+
                ", logL= "+utils::toString(logL)+", CV="+utils::toString(logLcv)+
                (!converged ? " did not converge" : params.best < logLcv ? " improved" : ""));
        }
        if(useCV) {  // we are searching for the highest cross-validation score
            if( params.best < logLcv && converged)
            {   // update the best-fit params and the starting point for fitting
                params.best = logLcv;
                params.ampl = result;
            }
            return -logLcv;
        } else {  // we are searching for the target value of logL
            double difference = params.targetLogL - logL;
            if(fabs(difference) < params.best && converged) {
                params.best = fabs(difference);
                params.ampl = result;
            }
            return difference;
        }
    }
    const SplineLogDensityFitter<N>& fitter;
    SplineLogFitParams& params;
};
}  // internal namespace

template<int N>
std::vector<double> splineLogDensity(const std::vector<double> &grid,
    const std::vector<double> &xvalues, const std::vector<double> &weights,
    FitOptions options, double smoothing)
{
    SplineLogFitParams params;
    const SplineLogDensityFitter<N> fitter(grid, xvalues,
        weights.empty()? std::vector<double>(xvalues.size(), 1./xvalues.size()) : weights,
        options, params);
    if(N==1) { // find the best-fit amplitudes without any smoothing
        std::vector<double> result(params.ampl);
        int numIter = findRootNdimDeriv(fitter, &params.ampl[0], 1e-8*params.gradNorm, 100, &result[0]);
        if(numIter>0)  // check for convergence
            params.ampl = result;
        utils::msg(utils::VL_VERBOSE, "splineLogDensity",
            "#iter="+utils::toString(numIter)+", logL="+utils::toString(fitter.logL(result))+
            ", CV="+utils::toString(fitter.logLcv(result))+(numIter<=0 ? " did not converge" : ""));
    } else {
        // Find the value of lambda and corresponding amplitudes that maximize the cross-validation score.
        // Normally lambda is a small number ( << 1), but it ranges from 0 to infinity,
        // so the root-finder uses a scaling transformation, such that scaledLambda=1
        // corresponds to lambda=0 and scaledLambda=0 -- to lambda=infinity.
        // However, we don't use the entire interval from 0 to infinity, to avoid singularities:
        const double MINSCALEDLAMBDA = 0.12422966;  // corresponds to lambda = 1000, rather arbitrary
        const double MAXSCALEDLAMBDA = 0.971884607; // corresponds to lambda = 1e-15
        // Since the minimizer first computes the function at the left endpoint of the interval
        // and then at the right endpoint, this leads to first performing an oversmoothed fit
        // (large lambda), which should yield a reasonable 'gaussian' first approximation,
        // then a fit with almost no smoothing, which starts with an already more reasonable
        // initial guess and thus has a better chance to converge.
        const SplineLogDensityLambdaFinder<N> finder(fitter, params);
        findMin(finder, MINSCALEDLAMBDA, MAXSCALEDLAMBDA, NAN, 1e-4);
        if(smoothing>0) {
            // target value of log-likelihood is allowed to be worse than
            // the best value for the case of no smoothing by an amount
            // that is proportional to the expected rms variation of logL
            params.best = smoothing * fitter.logLrms(params.ampl);
            params.targetLogL = fitter.logL(params.ampl) - params.best;
            findRoot(finder, MINSCALEDLAMBDA, MAXSCALEDLAMBDA, 1e-4);
        }
    }
    return fitter.getNormalizedAmplitudes(params.ampl);
}

// force the template instantiations to compile
template std::vector<double> splineLogDensity<1>(
    const std::vector<double>&, const std::vector<double>&, const std::vector<double>&, FitOptions, double);
template std::vector<double> splineLogDensity<3>(
    const std::vector<double>&, const std::vector<double>&, const std::vector<double>&, FitOptions, double);

//------------ GENERATION OF UNEQUALLY SPACED GRIDS ------------//

std::vector<double> createUniformGrid(unsigned int nnodes, double xmin, double xmax)
{
    if(nnodes<2 || xmax<=xmin)
        throw std::invalid_argument("Invalid parameters for grid creation");
    std::vector<double> grid(nnodes);
    for(unsigned int k=1; k<nnodes-1; k++)
        grid[k] = (xmin * (nnodes-1-k) + xmax * k) / (nnodes-1);
    grid.front() = xmin;
    grid.back()  = xmax;
    return grid;
}

std::vector<double> createExpGrid(unsigned int nnodes, double xmin, double xmax)
{
    if(nnodes<2 || xmin<=0 || xmax<=xmin)
        throw std::invalid_argument("Invalid parameters for grid creation");
    double logmin = log(xmin), logmax = log(xmax);
    std::vector<double> grid(nnodes);
    grid.front() = xmin;
    grid.back()  = xmax;
    for(unsigned int k=1; k<nnodes-1; k++)
        grid[k] = exp(logmin + k*(logmax-logmin)/(nnodes-1));
    return grid;
}

// Creation of grid with cells increasing first near-linearly, then near-exponentially
namespace{
class GridSpacingFinder: public IFunctionNoDeriv {
public:
    GridSpacingFinder(double _dynrange, int _nnodes) : dynrange(_dynrange), nnodes(_nnodes) {};
    virtual double value(const double A) const {
        return (A==0) ? nnodes-dynrange :
            (exp(A*nnodes)-1)/(exp(A)-1) - dynrange;
    }
private:
    double dynrange;
    int nnodes;
};
}

std::vector<double> createNonuniformGrid(unsigned int nnodes, double xmin, double xmax, bool zeroelem)
{   // create grid so that x_k = B*(exp(A*k)-1)
    if(nnodes<2 || xmin<=0 || xmax<=xmin)
        throw std::invalid_argument("Invalid parameters for grid creation");
    double A, B, dynrange=xmax/xmin;
    std::vector<double> grid(nnodes);
    int indexstart=zeroelem?1:0;
    if(zeroelem) {
        grid[0] = 0;
        nnodes--;
    }
    if(fcmp(static_cast<double>(nnodes), dynrange, 1e-6)==0) { // no need for non-uniform grid
        for(unsigned int i=0; i<nnodes; i++)
            grid[i+indexstart] = xmin+(xmax-xmin)*i/(nnodes-1);
        return grid;
    }
    // solve for A:  dynrange = (exp(A*nnodes)-1)/(exp(A)-1)
    GridSpacingFinder F(dynrange, nnodes);
    // first localize the root coarsely, to avoid overflows in root solver
    double Amin=0, Amax=0;
    double step=1;
    while(step>10./nnodes)
        step/=2;
    if(dynrange>nnodes) {
        while(Amax<10 && F(Amax)<=0)
            Amax+=step;
        Amin = Amax-step;
    } else {
        while(Amin>-10 && F(Amin)>=0)
            Amin-=step;
        Amax = Amin+step;
    }
    A = findRoot(F, Amin, Amax, 1e-4);
    B = xmin / (exp(A)-1);
    for(unsigned int i=0; i<nnodes; i++)
        grid[i+indexstart] = B*(exp(A*(i+1))-1);
    grid[nnodes-1+indexstart] = xmax;
    return grid;
}

/// creation of a grid with minimum guaranteed number of input points per bin
namespace{
void makegrid(std::vector<double>::iterator begin, std::vector<double>::iterator end,
    double startval, double endval)
{
    double step=(endval-startval)/(end-begin-1);
    while(begin!=end){
        *begin=startval;
        startval+=step;
        ++begin;
    }
    *(end-1)=endval;  // exact value
}
}

std::vector<double> createAlmostUniformGrid(unsigned int gridsize,
    const std::vector<double> &srcpoints_unsorted, unsigned int minbin)
{
    if(srcpoints_unsorted.size()==0)
        throw std::invalid_argument("createAlmostUniformGrid: input points array is empty");
    if(gridsize < 2 || (gridsize-1)*minbin > srcpoints_unsorted.size())
        throw std::invalid_argument("createAlmostUniformGrid: invalid grid size");
    std::vector<double> srcpoints(srcpoints_unsorted);
    std::sort(srcpoints.begin(), srcpoints.end());
    std::vector<double> grid(gridsize);
    std::vector<double>::iterator gridbegin=grid.begin(), gridend=grid.end();
    std::vector<double>::const_iterator srcbegin=srcpoints.begin(), srcend=srcpoints.end();
    std::vector<double>::const_iterator srciter;
    std::vector<double>::iterator griditer;
    bool ok=true, directionBackward=false;
    int numChangesDirection=0;
    do{
        makegrid(gridbegin, gridend, *srcbegin, *(srcend-1));
        ok=true;
        // find the index of bin with the largest number of points
        int largestbin=-1;
        unsigned int maxptperbin=0;
        for(srciter=srcbegin, griditer=gridbegin; griditer!=gridend-1; ++griditer) {
            unsigned int ptperbin=0;
            while(srciter+ptperbin!=srcend && *(srciter+ptperbin) < *(griditer+1))
                ++ptperbin;
            if(ptperbin>maxptperbin) {
                maxptperbin=ptperbin;
                largestbin=griditer-grid.begin();
            }
            srciter+=ptperbin;
        }
        // check that all bins contain at least minbin srcpoints
        if(!directionBackward) {  // forward scan
            srciter = srcbegin;
            griditer = gridbegin;
            while(ok && griditer!=gridend-1) {
                unsigned int ptperbin=0;
                while(srciter+ptperbin!=srcend && *(srciter+ptperbin) < *(griditer+1))
                    ptperbin++;
                if(ptperbin>=minbin)  // ok, move to the next one
                {
                    ++griditer;
                    srciter+=ptperbin;
                } else {  // assign minbin points and decrease the available grid interval from the front
                    if(griditer-grid.begin() < largestbin) {
                        // bad bin is closer to the grid front; move gridbegin forward
                        while(ptperbin<minbin && srciter+ptperbin!=srcend)
                            ptperbin++;
                        if(srciter+ptperbin==srcend)
                            directionBackward=true; // oops, hit the end of array..
                        else {
                            srcbegin=srciter+ptperbin;
                            gridbegin=griditer+1;
                        }
                    } else {
                        directionBackward=true;
                    }   // will restart scanning from the end of the grid
                    ok=false;
                }
            }
        } else {  // backward scan
            srciter = srcend-1;
            griditer = gridend-1;
            while(ok && griditer!=gridbegin) {
                unsigned int ptperbin=0;
                while(srciter+1-ptperbin!=srcbegin && *(srciter-ptperbin) >= *(griditer-1))
                    ptperbin++;
                if(ptperbin>=minbin)  // ok, move to the previous one
                {
                    --griditer;
                    if(srciter+1-ptperbin==srcbegin)
                        srciter=srcbegin;
                    else
                        srciter-=ptperbin;
                } else {  // assign minbin points and decrease the available grid interval from the back
                    if(griditer-grid.begin() <= largestbin) {
                        // bad bin is closer to the grid front; reset direction to forward
                        directionBackward=false;
                        numChangesDirection++;
                        if(numChangesDirection>10) {
                            utils::msg(utils::VL_DEBUG, FUNCNAME, "grid creation seems not to converge?");
                            return grid;  // don't run forever but would not fulfill the minbin condition
                        }
                    } else {
                        // move gridend backward
                        while(ptperbin<minbin && srciter-ptperbin!=srcbegin)
                            ++ptperbin;
                        if(srciter-ptperbin==srcbegin) {
                            directionBackward=false;
                            numChangesDirection++;
                            if(numChangesDirection>10) {
                                utils::msg(utils::VL_WARNING,
                                    "createAlmostUniformGrid", "grid creation does not seem to converge");
                                return grid;  // don't run forever but would not fulfill the minbin condition
                            }
                        } else {
                            srcend=srciter-ptperbin+1;
                            gridend=griditer;
                        }
                    }
                    ok=false;
                }
            }
        }
    } while(!ok);
    return grid;
}

std::vector<double> mirrorGrid(const std::vector<double> &input)
{
    unsigned int size = input.size();
    if(size==0 || input[0]!=0)
        throw std::invalid_argument("incorrect input in mirrorGrid");
    std::vector<double> output(size*2-1);
    output[size-1] = 0;
    for(unsigned int i=1; i<size; i++) {
        if(input[i] <= input[i-1])
            throw std::invalid_argument("incorrect input in mirrorGrid");
        output[size-1-i] = -input[i];
        output[size-1+i] =  input[i];
    }
    return output;
}

std::vector<double> createInterpolationGrid(const IFunction& fnc, double eps)
{
    // restrict the search to |x|<=xmax, assuming that x=log(something)
    const double xmax = 100.;  // exp(xmax) ~ 2.7e43
    // initial trial points
    const int NUMTRIAL = 10;
    const double XINIT[NUMTRIAL] = {-75., -50., -30., -15., -3., 3., 15., 30., 50., 75.};
    double xinit=0., d2f0 = 0.;
    // pick up the initial point where the second derivative is the highest
    for(int k=0; k<NUMTRIAL; k++) {
        PointNeighborhood f0(fnc, XINIT[k]);
        if(isFinite(f0.fder2) && fabs(f0.fder2) > fabs(d2f0)) {
            xinit = XINIT[k];
            d2f0  = f0.fder2;
        }
    }
    // tolerance parameter translated into the initial stepsize
    const double eps4 = sqrt(sqrt(eps*(384./5)));
    PointNeighborhood fm(fnc, xinit-eps4);
    PointNeighborhood fp(fnc, xinit+eps4);
    double d2fm = fm.fder2, d2fp = fp.fder2;
    double d3f0 = (d2f0-d2fm) / eps4, d3fp = (d2fp-d2f0) / eps4;
    double dx = -eps4;
    double x  = xinit;
    d2fp = d2f0;
    std::vector<double> result(1, xinit);
    // we first scan the range of x from xinit down,
    // then reverse the direction of scan and restart from xinit up, then stop
    int stage=0;
    while(stage<2) {
        x += dx;
        PointNeighborhood fx(fnc, x);
        double d2f = fx.fder2;
        double d3f = (d2f-d2fp) / dx;
        double dif = fabs((d3f-d3fp) / dx) + 0.1 * (fabs(d3fp) + fabs(d3f));  // estimate of 4th derivative
        double sgn = (stage*2-1);  // -1 for inward scan, +1 for outward
        dx         = eps4 / fmin(sqrt(sqrt(dif)), 1.) * sgn;
        result.push_back(x);
        // we have reached the asymptotic linear regime if the second derivative is close enough to zero
        // (check both the current and the previous value to avoid triggering this condition prematurely)
        if((fabs(d2f) < eps && fabs(d2fp) < eps) || fabs(x) > xmax || !isFinite(d2f+dx)) {
            if(stage==0) {
                std::reverse(result.begin(), result.end());
                x    = xinit;
                dx   = eps4;
                d2fp = d2f0;
                d3fp = d3f0;
            }
            ++stage;
        } else {
            d2fp = d2f;
            d3fp = d3f;
        }
    }
    utils::msg(utils::VL_DEBUG, "createInterpolationGrid", "Grid: [" +
        utils::toString(result.front()) + ":" + utils::toString(result.back()) + "], " +
        utils::toString(result.size()) + " nodes");
    return result;
}

}  // namespace

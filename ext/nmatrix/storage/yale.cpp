/////////////////////////////////////////////////////////////////////
// = NMatrix
//
// A linear algebra library for scientific computation in Ruby.
// NMatrix is part of SciRuby.
//
// NMatrix was originally inspired by and derived from NArray, by
// Masahiro Tanaka: http://narray.rubyforge.org
//
// == Copyright Information
//
// SciRuby is Copyright (c) 2010 - 2013, Ruby Science Foundation
// NMatrix is Copyright (c) 2013, Ruby Science Foundation
//
// Please see LICENSE.txt for additional copyright notices.
//
// == Contributing
//
// By contributing source code to SciRuby, you agree to be bound by
// our Contributor Agreement:
//
// * https://github.com/SciRuby/sciruby/wiki/Contributor-Agreement
//
// == yale.c
//
// "new yale" storage format for 2D matrices (like yale, but with
// the diagonal pulled out for O(1) access).
//
// Specifications:
// * dtype and index dtype must necessarily differ
//      * index dtype is defined by whatever unsigned type can store
//        max(rows,cols)
//      * that means vector ija stores only index dtype, but a stores
//        dtype
// * vectors must be able to grow as necessary
//      * maximum size is rows*cols+1

/*
 * Standard Includes
 */

#include <ruby.h>
#include <algorithm>  // std::min
#include <cstdio>     // std::fprintf
#include <iostream>
#include <array>
#include <typeinfo>
#include <tuple>
#include <queue>

#define RB_P(OBJ) \
	rb_funcall(rb_stderr, rb_intern("print"), 1, rb_funcall(OBJ, rb_intern("object_id"), 0)); \
	rb_funcall(rb_stderr, rb_intern("puts"), 1, rb_funcall(OBJ, rb_intern("inspect"), 0));

/*
 * Project Includes
 */

// #include "types.h"
#include "data/data.h"
#include "math/math.h"

#include "common.h"
#include "yale.h"

#include "nmatrix.h"
#include "ruby_constants.h"

/*
 * Macros
 */

#ifndef NM_MAX
#define NM_MAX(a,b) (((a)>(b))?(a):(b))
#define NM_MIN(a,b) (((a)<(b))?(a):(b))
#endif

#ifndef NM_MAX_ITYPE
#define NM_MAX_ITYPE(a,b) ((static_cast<int8_t>(a) > static_cast<int8_t>(b)) ? static_cast<nm::itype_t>(a) : static_cast<nm::itype_t>(b))
#define NM_MIN_ITYPE(a,b) ((static_cast<int8_t>(a) < static_cast<int8_t>(b)) ? static_cast<nm::itype_t>(a) : static_cast<nm::itype_t>(b))
#endif

/*
 * Forward Declarations
 */

extern "C" {
  static YALE_STORAGE*  nm_copy_alloc_struct(const YALE_STORAGE* rhs, const nm::dtype_t new_dtype, const size_t new_capacity, const size_t new_size);
  static YALE_STORAGE*	alloc(nm::dtype_t dtype, size_t* shape, size_t dim, nm::itype_t min_itype);

  static size_t yale_count_slice_copy_ndnz(const YALE_STORAGE* s, size_t*, size_t*);

  static void* default_value_ptr(const YALE_STORAGE* s);
  static VALUE default_value(const YALE_STORAGE* s);
  static VALUE obj_at(YALE_STORAGE* s, size_t k);

  /* Ruby-accessible functions */
  static VALUE nm_size(VALUE self);
  static VALUE nm_a(int argc, VALUE* argv, VALUE self);
  static VALUE nm_d(int argc, VALUE* argv, VALUE self);
  static VALUE nm_lu(VALUE self);
  static VALUE nm_ia(VALUE self);
  static VALUE nm_ja(VALUE self);
  static VALUE nm_ija(int argc, VALUE* argv, VALUE self);

  static VALUE nm_nd_row(int argc, VALUE* argv, VALUE self);

  static inline size_t src_ndnz(const YALE_STORAGE* s) {
    return reinterpret_cast<YALE_STORAGE*>(s->src)->ndnz;
  }

} // end extern "C" block

namespace nm { namespace yale_storage {


template <typename DType, typename IType>
static bool						ndrow_is_empty(const YALE_STORAGE* s, IType ija, const IType ija_next);

template <typename LDType, typename RDType, typename IType>
static bool						ndrow_eqeq_ndrow(const YALE_STORAGE* l, const YALE_STORAGE* r, IType l_ija, const IType l_ija_next, IType r_ija, const IType r_ija_next);

template <typename LDType, typename RDType, typename IType>
static bool           eqeq(const YALE_STORAGE* left, const YALE_STORAGE* right);

template <typename LDType, typename RDType, typename IType>
static bool eqeq_different_defaults(const YALE_STORAGE* s, const LDType& s_init, const YALE_STORAGE* t, const RDType& t_init);

template <typename IType>
static YALE_STORAGE*	copy_alloc_struct(const YALE_STORAGE* rhs, const dtype_t new_dtype, const size_t new_capacity, const size_t new_size);

template <typename IType>
static void						increment_ia_after(YALE_STORAGE* s, IType ija_size, IType i, long n);

template <typename IType>
static void           c_increment_ia_after(YALE_STORAGE* s, size_t ija_size, size_t i, long n) {
  increment_ia_after<IType>(s, ija_size, i, n);
}

template <typename IType>
static IType				  insert_search(YALE_STORAGE* s, IType left, IType right, IType key, bool* found);

template <typename DType, typename IType>
static char           vector_insert(YALE_STORAGE* s, size_t pos, size_t* j, void* val_, size_t n, bool struct_only);

template <typename DType, typename IType>
static char           vector_insert_resize(YALE_STORAGE* s, size_t current_size, size_t pos, size_t* j, size_t n, bool struct_only);

template <typename DType, typename IType>
static std::tuple<long,bool,std::queue<std::tuple<IType,IType,int> > > count_slice_set_ndnz_change(YALE_STORAGE* s, size_t* coords, size_t* lengths, DType* v, size_t v_size);

template <typename IType>
static inline IType* IJA(const YALE_STORAGE* s) {
  return reinterpret_cast<IType*>(reinterpret_cast<YALE_STORAGE*>(s->src)->ija);
}

template <typename IType>
static inline IType IJA_SET(const YALE_STORAGE* s, size_t loc, IType val) {
  return IJA<IType>(s)[loc] = val;
}

template <typename DType>
static inline DType* A(const YALE_STORAGE* s) {
  return reinterpret_cast<DType*>(reinterpret_cast<YALE_STORAGE*>(s->src)->a);
}

template <typename DType>
static inline DType A_SET(const YALE_STORAGE* s, size_t loc, DType val) {
  return A<DType>(s)[loc] = val;
}


/*
 * Functions
 */

/*
 * Copy a vector from one IType or DType to another.
 */
template <typename LType, typename RType>
static inline void copy_recast_vector(const void* in_, void* out_, size_t length) {
  const RType* in = reinterpret_cast<const RType*>(in_);
  LType* out      = reinterpret_cast<LType*>(out_);
  for (size_t i = 0; i < length; ++i) {
    out[i] = in[i];
  }
  out;
}


static inline void copy_recast_itype_vector(const void* in, nm::itype_t in_itype, void* out, nm::itype_t out_itype, size_t length) {
  NAMED_LR_ITYPE_TEMPLATE_TABLE(ttable, copy_recast_vector, void, const void* in_, void* out_, size_t length);

  ttable[out_itype][in_itype](in, out, length);
}


/*
 * Create Yale storage from IA, JA, and A vectors given in Old Yale format (probably from a file, since NMatrix only uses
 * new Yale for its storage).
 *
 * This function is needed for Matlab .MAT v5 IO.
 */
template <typename LDType, typename RDType, typename IType>
YALE_STORAGE* create_from_old_yale(dtype_t dtype, size_t* shape, void* r_ia, void* r_ja, void* r_a) {
  IType*  ir = reinterpret_cast<IType*>(r_ia);
  IType*  jr = reinterpret_cast<IType*>(r_ja);
  RDType* ar = reinterpret_cast<RDType*>(r_a);

  // Read through ia and ja and figure out the ndnz (non-diagonal non-zeros) count.
  size_t ndnz = 0, i, p, p_next;

  for (i = 0; i < shape[0]; ++i) { // Walk down rows
    for (p = ir[i], p_next = ir[i+1]; p < p_next; ++p) { // Now walk through columns

      if (i != jr[p]) ++ndnz; // entry is non-diagonal and probably nonzero

    }
  }

  // Having walked through the matrix, we now go about allocating the space for it.
  YALE_STORAGE* s = alloc(dtype, shape, 2, UINT8);

  s->capacity = shape[0] + ndnz + 1;
  s->ndnz     = ndnz;

  // Setup IJA and A arrays
  s->ija = ALLOC_N( IType, s->capacity );
  s->a   = ALLOC_N( LDType, s->capacity );
  IType* ijl    = reinterpret_cast<IType*>(s->ija);
  LDType* al    = reinterpret_cast<LDType*>(s->a);

  // set the diagonal to zero -- this prevents uninitialized values from popping up.
  for (size_t index = 0; index < shape[0]; ++index) {
    al[index] = 0;
  }

  // Figure out where to start writing JA in IJA:
  size_t pp = s->shape[0]+1;

  // Find beginning of first row
  p = ir[0];

  // Now fill the arrays
  for (i = 0; i < s->shape[0]; ++i) {

    // Set the beginning of the row (of output)
    ijl[i] = pp;

    // Now walk through columns, starting at end of row (of input)
    for (size_t p_next = ir[i+1]; p < p_next; ++p, ++pp) {

      if (i == jr[p]) { // diagonal

        al[i] = ar[p];
        --pp;

      } else {          // nondiagonal

        ijl[pp] = jr[p];
        al[pp]  = ar[p];

      }
    }
  }

  ijl[i] = pp; // Set the end of the last row

  // Set the zero position for our output matrix
  al[i] = 0;

  return s;
}


/*
 * Empty the matrix by initializing the IJA vector and setting the diagonal to 0.
 *
 * Called when most YALE_STORAGE objects are created.
 */
template <typename DType, typename IType>
void init(YALE_STORAGE* s, void* init_val) {
  IType IA_INIT = s->shape[0] + 1;

  IType* ija = reinterpret_cast<IType*>(s->ija);
  // clear out IJA vector
  for (IType i = 0; i < IA_INIT; ++i) {
    ija[i] = IA_INIT; // set initial values for IJA
  }

  clear_diagonal_and_zero<DType>(s, init_val);
}

size_t max_size(YALE_STORAGE* s) {
  size_t result = s->shape[0]*s->shape[1] + 1;
  if (s->shape[0] > s->shape[1])
    result += s->shape[0] - s->shape[1];

  return result;
}


///////////////
// Accessors //
///////////////


/*
 * Determine the number of non-diagonal non-zeros in a not-yet-created copy of a slice or matrix.
 */
template <typename DType, typename IType>
static size_t count_slice_copy_ndnz(const YALE_STORAGE* s, size_t* offset, size_t* shape) {
  IType* ija = reinterpret_cast<IType*>(s->ija);
  DType* a   = reinterpret_cast<DType*>(s->a);

  DType ZERO(*reinterpret_cast<DType*>(default_value_ptr(s)));

  // Calc ndnz for the destination
  size_t ndnz  = 0;
  size_t i, j; // indexes of destination matrix
  size_t k, l; // indexes of source matrix
  for (i = 0; i < shape[0]; i++) {
    k = i + offset[0];
    for (j = 0; j < shape[1]; j++) {
      l = j + offset[1];

      if (j == i)  continue;

      if (k == l) { // for diagonal element of source
        if (a[k] != ZERO) ++ndnz;
      } else { // for non-diagonal element
        for (size_t c = ija[k]; c < ija[k+1]; c++) {
          if (ija[c] == l) {
            ++ndnz;
            break;
          }
        }
      }
    }
  }

  return ndnz;
}



/*
 * Copy some portion of a matrix into a new matrix.
 */
template <typename LDType, typename RDType, typename IType>
static void slice_copy(YALE_STORAGE* ns, const YALE_STORAGE* s, size_t* offset, size_t* lengths, dtype_t new_dtype) {

  IType* src_ija = reinterpret_cast<IType*>(s->ija);
  RDType* src_a  = reinterpret_cast<RDType*>(s->a);

  RDType RZERO(*reinterpret_cast<RDType*>(default_value_ptr(s)));

   // Initialize the A and IJA arrays
  LDType val(RZERO); // need default value for init. Can't use ns default value because it's not initialized yet
  init<LDType,IType>(ns, &val);
  IType*  dst_ija = reinterpret_cast<IType*>(ns->ija);
  LDType* dst_a   = reinterpret_cast<LDType*>(ns->a);

  size_t ija  = lengths[0] + 1;

  size_t i, j; // indexes of destination matrix
  size_t k, l; // indexes of source matrix

  for (i = 0; i < lengths[0]; ++i) {
    k = i + offset[0];
    for (j = 0; j < lengths[1]; ++j) {
      bool found = false;
      l = j + offset[1];

      // Get value from source matrix
      if (k == l) { // source diagonal
        if (src_a[k] != RZERO) { // don't bother copying non-zero values from the diagonal
          val = src_a[k];
          found = true;
        }
      } else {
        // copy one non-diagonal element
        for (size_t c = src_ija[k]; !found && c < src_ija[k+1]; ++c) {
          if (src_ija[c] == l) {
            val   = src_a[c];
            found = true;
          }
        }
      }

      if (found) {
        // Set value in destination matrix
        if (i == j) {
          dst_a[i] = val;
        } else {
          // copy non-diagonal element
          dst_ija[ija] = j;
          dst_a[ija]   = val;
          ++ija;
          for (size_t c = i + 1; c <= lengths[0]; ++c) {
            dst_ija[c] = ija;
          }
        }
      }
    }
  }

  dst_ija[lengths[0]] = ija; // indicate the end of the last row
  ns->ndnz            = ija - lengths[0] - 1; // update ndnz count
}


/*
 * Get a single element of a yale storage object
 */
template <typename DType, typename IType>
static void* get_single(YALE_STORAGE* storage, SLICE* slice) {
  YALE_STORAGE* s = reinterpret_cast<YALE_STORAGE*>(storage->src);

  DType* a   = reinterpret_cast<DType*>(s->a);
  IType* ija = reinterpret_cast<IType*>(s->ija);

  size_t coord0 = storage->offset[0] + slice->coords[0];
  size_t coord1 = storage->offset[1] + slice->coords[1];

  if (coord0 == coord1)
    return &(a[ coord0 ]); // return diagonal entry

  if (ija[coord0] == ija[coord0+1])
    return &(a[ s->shape[0] ]); // return zero pointer

  // binary search for the column's location
  int pos = binary_search<IType>(s, ija[coord0], ija[coord0+1]-1, coord1);

  if (pos != -1 && ija[pos] == coord1)
    return &(a[pos]); // found exact value

  return &(a[ s->shape[0] ]); // return a pointer that happens to be zero
}


/*
 * Returns a pointer to the correct location in the A vector of a YALE_STORAGE object, given some set of coordinates
 * (the coordinates are stored in slice).
 */
template <typename DType,typename IType>
void* ref(YALE_STORAGE* s, SLICE* slice) {

  YALE_STORAGE* ns = ALLOC( YALE_STORAGE );

  ns->dim     = s->dim;
  ns->offset  = ALLOC_N(size_t, ns->dim);
  ns->shape   = ALLOC_N(size_t, ns->dim);

  for (size_t i = 0; i < ns->dim; ++i) {
    ns->offset[i]   = slice->coords[i] + s->offset[i];
    ns->shape[i]    = slice->lengths[i];
  }

  ns->dtype   = s->dtype;
  ns->itype   = s->itype; // or should we go by shape?

  ns->a       = reinterpret_cast<YALE_STORAGE*>(s->src)->a;
  ns->ija     = reinterpret_cast<YALE_STORAGE*>(s->src)->ija;

  ns->src     = s->src;
  s->src->count++;

  ns->ndnz    = 0;
  ns->capacity= 0;

  return ns;

}

/*
 * Copy a vector into a temporary location, changing certain entries.
 *
 * Note that s must not be a reference. Call this on s->src
 */
template <typename DType, typename IType>
static void modify_partial_row(YALE_STORAGE* s, IType si, IType pos, IType end, int n, size_t j_off, size_t j_len, DType* v, size_t v_size, size_t& k) {
  int new_len   = end - pos + n;
  IType* tmp_ja = ALLOCA_N(IType, new_len);
  DType* tmp_a  = ALLOCA_N(DType, new_len);

  IType* s_ija  = reinterpret_cast<IType*>(s->ija);
  DType* s_a    = reinterpret_cast<DType*>(s->a);

  DType ZERO(*reinterpret_cast<DType*>(default_value_ptr(s)));

  IType m = 0, p = pos;
  for (IType j = 0; j < j_len; ++j, ++k) {
    IType sj   = j + j_off;
    IType x_sj = s_ija[p];

    if (sj == si) {              // set diagonal in the real A
      s_a[si] = v[k % v_size];
    } else if (sj < x_sj) {
      if (v[k % v_size] != ZERO) { // add non-zero
        tmp_ja[m] = sj;
        tmp_a[m]  = v[k % v_size];
        ++m;
      }
    } else { // sj == x_sj
      if (v[k % v_size] != ZERO) {
        tmp_ja[m] = sj;
        tmp_a[m]  = v[k % v_size];
        ++m;
      }
      ++p;
      x_sj = p < end ? s_ija[p] : (IType)(s->shape[1]);
    }
  }

  // Now write into the original A and IJA.
  for (IType m = 0; m < new_len; ++m) {
    s_a[pos + m]   = tmp_a[m];
    s_ija[pos + m] = tmp_ja[m];
  }

  // Update IA array
  increment_ia_after<IType>(s, s->shape[0], si, s_ija[pos+1] - s_ija[pos] + n);
}


/*
 * Return as a pair:
 *    - The total ndnz change of some slice-set operation;
 *    - Whether the vector should be resized regardless (which may simply be because the total size of partial copies
 *      that would have to be done to accomplish the slice-set conventionally exceeds the copy size for simply rewriting
 *      the vectors; and by conventionally, I mean moving parts of A[x...] and IJA[x...] as we add new rows).
 *
 * By default, v is a single element of type DType. We iterate through it repeatedly, using v[k] as the element to which
 * we want to set the current element of s. Each time we reach the end of v, we go back to the beginning.
 *
 * FIXME: There is one obvious potential improvement here: that changes within a row do not necessarily need a copy/resize if
 * FIXME: they are balanced out by changes in a different row, especially one which is located very nearby. But this is
 * FIXME: an edge case, and very complicated to implement, so I have elected not to address it.
 *
 * FIXME: I just came up with another improvement. The first part of the IA vector has to be rewritten following the
 * FIXME: first row i whose size gets changed. This should be taken into account.
 */
template <typename DType, typename IType>
static std::tuple<long,bool,std::queue<std::tuple<IType,IType,int> > > count_slice_set_ndnz_change(YALE_STORAGE* s, size_t* coords, size_t* lengths, DType* v, size_t v_size) {
  long change = 0;
  std::queue<std::tuple<IType,IType,int> > row_plan_q;
  unsigned long partial_copies = 0;
  bool resize = false;

  IType* ija  = IJA<IType>(s);
  DType* a    = A<DType>(s);

  DType ZERO(*reinterpret_cast<DType*>(default_value_ptr(s)));
  IType last_ija = get_size<IType>(s);

  size_t k = 0; // index in v (actually, use k % v_size for simplicity, so we don't have to keep resetting to 0)

  for (IType i = 0; i < lengths[0]; ++i) {
    IType si = coords[0] + s->offset[0] + i;

    IType row_start      = ija[si];
    IType past_row_end   = ija[si+1];
    int   row_change     = 0;

    if (row_start == ija[si+1]) { // empty row

      for (IType j = 0; j < lengths[1]; ++j, ++k) {
        IType sj = coords[1] + s->offset[1] + i;

        if (sj == si) continue; // don't need to count diagonals

        // If the thing we want to add is non-zero, we need to update.
        if (v[k % v_size] != ZERO) row_change++;
      }

    } else { // non-empty row.
      IType p      = binary_search_left_boundary<IType>(s, row_start, past_row_end-1, s->offset[1]);
      row_start    = p;
      IType x_sj   = p < past_row_end ? ija[p] : (IType)(s->shape[1]); // next existent sj if it exists (otherwise, use one past end)

      for (IType j = 0; j < lengths[1]; ++j, ++k) {
        IType sj      = j + s->offset[1] + coords[1];

        if (sj != si) { // don't need to count diagonals
          if (sj < x_sj) { // anything we find in this condition is an add
            if (v[k % v_size] != ZERO) row_change++;
          } else if (sj == x_sj) { // could be a change (don't count it) or a remove (count it)
            if (v[k % v_size] == ZERO) row_change--;
            ++p;  // need to step to the next x_sj
            x_sj   = p < past_row_end ? ija[p] : (IType)(s->shape[1]); // next existent sj if it exists (otherwise, use one past end)
          }
        }
      }
      past_row_end  = p;
    }

    // We need to copy from the next row (s->ija[si+1]) to the end (last_ija) as well as changing the row entries.
    if (row_change != 0) {
      partial_copies += last_ija - ija[si+1] + row_change;
      change += row_change;

      // There's no reason to do it conventionally if partial_copies is more than the max_size
      if (partial_copies >= last_ija) resize = true;
    }

    row_plan_q.push(std::make_tuple(row_start, past_row_end, row_change));
  }

  return make_tuple(change, resize, row_plan_q);
}



/*
 * Attempt to set multiple cells in a YALE_STORAGE object.
 */
template <typename DType, typename IType>
static void set_multiple_cells(YALE_STORAGE* s, size_t* coords, size_t* lengths, DType* v, size_t v_size) {
  typedef std::tuple<IType,IType,int>               RowPlan;
  typedef std::queue<std::tuple<IType,IType,int> >  PlanQ;
  typedef std::tuple<bool,long,PlanQ>               Plan;

  IType size                = get_size<IType>(s);
  // First count the number of entries that we expect will be added or removed and determine if a resize would be
  // more efficient.
  long delta_size;
  bool copy;
  PlanQ row_plans;
  Plan plan = count_slice_set_ndnz_change<DType, IType>(s, coords, lengths, v, v_size);
  std::tie(delta_size, copy, row_plans) = plan;

  std::cerr << "copy = " << int(copy) << ", delta_size = " << int(delta_size) << ", row_plans size = " << row_plans.size() << std::endl;


  IType* ija = IJA<IType>(s);
  DType* a   = A<DType>(s);

  // We don't *have* to shrink, but we might as well.
  if (!copy && delta_size < 0 && size + delta_size < s->capacity / GROWTH_CONSTANT)
    copy = true;

  std::cerr << "copy = " << int(copy) << std::endl;

  if (!copy) {
    size_t k  = 0;
    long accum = 0; // counts the accumulated error in row_start (the first element of row_plan_q's pair) as we modify.
    IType si   = s->offset[0] + coords[0]; // use as row counter

    while (!row_plans.empty()) { // for each row
      IType pos, end;
      int n;
      std::tie(pos, end, n) = row_plans.front(); row_plans.pop();
      pos += accum; end += accum;

      std::cerr << "row " << int(si) << "\tpos=" << int(pos) << "\tend=" << int(end) << "\tn=" << n << std::endl;

      if (n > 0) { // size increase: copy later rows first, then modify contents of this row.
        for (IType m = 0; m < size - end; ++m) { // copy/resize
          ija[size+n-1-m] = ija[size-1-m];
          a[size+n-1-m]   = a[size-1-m];
        }

        modify_partial_row<DType,IType>((YALE_STORAGE*)(s->src), si, pos, end, n, s->offset[1] + coords[1], lengths[1], v, v_size, k);
      } else if (n < 0) { // size decrease: modify contents of row, then copy later rows
        modify_partial_row<DType,IType>((YALE_STORAGE*)(s->src), si, pos, end, n, s->offset[1] + coords[1], lengths[1], v, v_size, k);

        for (IType m = 0; m < size - end; ++m) {
          ija[size+n-1-m] = ija[size-1-m];
          a[size+n-1-m]   = a[size-1-m];
        }
      } else if (n == 0) { // just overwrite the row, no copy needed
        // FIXME: This can be done in-place.
        modify_partial_row<DType,IType>((YALE_STORAGE*)(s->src), si, pos, end, n, s->offset[1] + coords[1], lengths[1], v, v_size, k);
      }

      accum     += n;
      ++si;
    }
  } else {  // allowed to make a copy.
    size_t new_capacity = NM_MAX(s->capacity * GROWTH_CONSTANT, s->capacity + delta_size);
    size_t max_capacity = max_size(s);
    if (new_capacity > max_capacity) {
      new_capacity = max_capacity;

      if (s->capacity + delta_size > max_capacity)
        rb_raise(rb_eNoMemError, "insertion size exceeded maximum yale matrix size");
    }

    // Allocate new vectors. (ija and a local variables have the old ones)
    reinterpret_cast<YALE_STORAGE*>(s->src)->ija = ALLOC_N(IType, new_capacity);
    reinterpret_cast<YALE_STORAGE*>(s->src)->a   = ALLOC_N(DType, new_capacity);

    size_t k  = 0;
    long accum = 0; // counts the accumulated error in row_start (the first element of row_plan_q's pair) as we modify.
    IType si   = s->offset[0] + coords[0]; // use as row counter
    IType last_end = 0;
    int n = 0;
    IType pos, end;

    while (!row_plans.empty()) { // for each row
      std::tie(pos, end, n) = row_plans.front(); row_plans.pop();
      pos += accum;
      end += accum;

      accum += n;

      // Copy all values prior to the insertion site to the new IJA and new A.
      for (size_t m = 0; m < pos; ++m) {
        IJA_SET(s, m, ija[m]);
        A_SET(  s, m, a[m]);
      }

      // Fill in between pos and end
      modify_partial_row<DType,IType>((YALE_STORAGE*)(s->src), si, pos, end, n, s->offset[1] + coords[1], lengths[1], v, v_size, k);

      last_end = end;
    }

    // Copy values subsequent to insertion site
    for (size_t m = last_end; m < size; ++m) {
      IJA_SET(s, m+n, ija[m]);
      A_SET(  s, m+n, a[m]);
    }

    reinterpret_cast<YALE_STORAGE*>(s->src)->capacity = new_capacity;

    // Delete
    xfree(ija);
    xfree(a);
  }

  reinterpret_cast<YALE_STORAGE*>(s->src)->ndnz += delta_size;

}


/*
 * Attempt to set a single cell in a YALE_STORAGE object.
 */
template <typename DType, typename IType>
static void set_single_cell(YALE_STORAGE* storage, size_t* coords, DType& v) {
  YALE_STORAGE* s = reinterpret_cast<YALE_STORAGE*>(storage->src);

  size_t coord0 = storage->offset[0] + coords[0],
         coord1 = storage->offset[1] + coords[1];

  bool found = false;

  if (coord0 == coord1) {
    reinterpret_cast<DType*>(s->a)[coord0] = v; // set diagonal
    return;
  }

  // Get IJA positions of the beginning and end of the row
  if (reinterpret_cast<IType*>(storage->ija)[coord0] == reinterpret_cast<IType*>(storage->ija)[coord0+1]) {
    // empty row
    vector_insert<DType,IType>(s, reinterpret_cast<IType*>(s->ija)[coord0], &(coord1), &v, 1, false);
    increment_ia_after<IType>(s, s->shape[0], coord0, 1);
    s->ndnz++;

    return;
  }

  // non-empty row. search for coords[1] in the IJA array, between ija and ija_next
  // (including ija, not including ija_next)
  //ija_size = get_size<IType>(storage);

  // Do a binary search for the column
  size_t pos = insert_search<IType>(s,
                                    reinterpret_cast<IType*>(s->ija)[coord0],
                                    reinterpret_cast<IType*>(s->ija)[coord0+1]-1,
                                    coord1, &found);

  if (found) { // replace
    reinterpret_cast<IType*>(s->ija)[pos] = coord1;
    reinterpret_cast<DType*>(s->a)[pos]   = v;

    return;
  }

  vector_insert<DType,IType>(s, pos, &(coord1), &v, 1, false);
  increment_ia_after<IType>(s, s->shape[0], coord0, 1);
  s->ndnz++;

}


/*
 * Attempt to set a cell or cells in a Yale matrix.
 */
template <typename DType, typename IType>
void set(VALUE left, SLICE* slice, VALUE right) {
  YALE_STORAGE* storage = NM_STORAGE_YALE(left);

  // TODO: Easily modified to accept pass dense storage elements in instead of v (below). Won't work with slices.
  if (TYPE(right) == T_DATA) {
    if (RDATA(right)->dfree == (RUBY_DATA_FUNC)nm_delete || RDATA(right)->dfree == (RUBY_DATA_FUNC)nm_delete_ref) {
      rb_raise(rb_eNotImpError, "this type of slicing not yet supported");
    } else {
      rb_raise(rb_eTypeError, "unrecognized type for slice assignment");
    }

  } else {

    DType* v;
    size_t v_size = 1;
    if (TYPE(right) == T_ARRAY) {  // Allow the user to pass in an array
      v_size = RARRAY_LEN(right);
      v      = ALLOC_N(DType, v_size);
      for (size_t m = 0; m < RARRAY_LEN(right); ++m) {
        rubyval_to_cval(rb_ary_entry(right, m), storage->dtype, &(v[m]));
      }
    } else {
      v = reinterpret_cast<DType*>(rubyobj_to_cval(right, storage->dtype));
    }

    if (slice->single || (slice->lengths[0] == 1 && slice->lengths[1] == 1)) { // set a single cell
      set_single_cell<DType,IType>(storage, slice->coords, *v);
    } else {
      set_multiple_cells<DType,IType>(storage, slice->coords, slice->lengths, v, v_size);
    }

    xfree(v);
  }
}

///////////
// Tests //
///////////

/*
 * Yale eql? -- for whole-matrix comparison returning a single value.
 */
template <typename LDType, typename RDType, typename IType>
static bool eqeq(const YALE_STORAGE* left, const YALE_STORAGE* right) {
  LDType l_init = reinterpret_cast<LDType*>(left->a )[left->shape[0] ];
  RDType r_init = reinterpret_cast<RDType*>(right->a)[right->shape[0]];

  // If the defaults are different between the two matrices, or if slicing is involved, use this other function instead:
  if (l_init != r_init || left->src != left || right->src != right)
    return eqeq_different_defaults<LDType,RDType,IType>(left, l_init, right, r_init);

  LDType* la = reinterpret_cast<LDType*>(left->a);
  RDType* ra = reinterpret_cast<RDType*>(right->a);

  // Compare the diagonals first.
  for (size_t index = 0; index < left->shape[0]; ++index) {
    if (la[index] != ra[index]) return false;
  }

  IType* lij = reinterpret_cast<IType*>(left->ija);
  IType* rij = reinterpret_cast<IType*>(right->ija);

  for (IType i = 0; i < left->shape[0]; ++i) {

  // Get start and end positions of row
    IType l_ija = lij[i],
          l_ija_next = lij[i+1],
          r_ija = rij[i],
          r_ija_next = rij[i+1];

    // Check to see if one row is empty and the other isn't.
    if (ndrow_is_empty<LDType,IType>(left, l_ija, l_ija_next)) {
      if (!ndrow_is_empty<RDType,IType>(right, r_ija, r_ija_next)) {
      	return false;
      }

    } else if (ndrow_is_empty<RDType,IType>(right, r_ija, r_ija_next)) {
    	// one is empty but the other isn't
      return false;

    } else if (!ndrow_eqeq_ndrow<LDType,RDType,IType>(left, right, l_ija, l_ija_next, r_ija, r_ija_next)) {
    	// Neither row is empty. Must compare the rows directly.
      return false;
    }

  }

  return true;
}



/*
 * Are two non-diagonal rows the same? We already know.
 */
template <typename LDType, typename RDType, typename IType>
static bool ndrow_eqeq_ndrow(const YALE_STORAGE* l, const YALE_STORAGE* r, IType l_ija, const IType l_ija_next, IType r_ija, const IType r_ija_next) {
  bool l_no_more = false, r_no_more = false;

  IType *lij = reinterpret_cast<IType*>(l->ija),
        *rij = reinterpret_cast<IType*>(r->ija);

  LDType* la = reinterpret_cast<LDType*>(l->a);
  RDType* ra = reinterpret_cast<RDType*>(r->a);

  IType l_ja = lij[l_ija],
        r_ja = rij[r_ija];
        
  IType ja = std::min(l_ja, r_ja);

  LDType LZERO = la[l->shape[0]];
  RDType RZERO = ra[r->shape[0]];

  while (!(l_no_more && r_no_more)) {
    if (l_ja == r_ja) {

      if (ra[r_ija] != la[l_ija]) return false; // Direct comparison

      ++l_ija;
      ++r_ija;

      if (l_ija < l_ija_next) {
      	l_ja = lij[l_ija];

      } else {
      	l_no_more = true;
      }

      if (r_ija < r_ija_next) {
      	r_ja = rij[r_ija];

      } else {
      	r_no_more = true;
      }

      ja = std::min(l_ja, r_ja);

    } else if (l_no_more || ja < l_ja) {

      if (ra[r_ija] != RZERO) return false;

      ++r_ija;
      if (r_ija < r_ija_next) {
      	// get next column
      	r_ja = rij[r_ija];
        ja = std::min(l_ja, r_ja);

      } else {
      	l_no_more = true;
      }

    } else if (r_no_more || ja < r_ja) {

      if (la[l_ija] != LZERO) return false;

      ++l_ija;
      if (l_ija < l_ija_next) {
      	// get next column
        l_ja = lij[l_ija];
        ja = std::min(l_ja, r_ja);
      } else {
      	l_no_more = true;
      }

    } else {
      std::fprintf(stderr, "Unhandled in eqeq: l_ja=%d, r_ja=%d\n", (int)l_ja, (int)r_ja);
    }
  }

	// every item matched
  return true;
}

/*
 * Is the non-diagonal portion of the row empty?
 */
template <typename DType, typename IType>
static bool ndrow_is_empty(const YALE_STORAGE* s, IType ija, const IType ija_next) {
  if (ija == ija_next) return true;

  DType* a = reinterpret_cast<DType*>(reinterpret_cast<YALE_STORAGE*>(s->src)->a);

	// do all the entries = zero?
  for (; ija < ija_next; ++ija) {
    if (a[ija] != 0) return false;
  }

  return true;
}

//////////
// Math //
//////////

#define YALE_IA(s) (reinterpret_cast<IType*>(s->ija))
#define YALE_IJ(s) (reinterpret_cast<IType*>(s->ija) + s->shape[0] + 1)
#define YALE_COUNT(yale) (yale->ndnz + yale->shape[0])

/////////////
// Utility //
/////////////


/*
 * Binary search for finding the beginning of a slice. Returns the position of the first element which is larger than
 * bound.
 */
template <typename IType>
IType binary_search_left_boundary(const YALE_STORAGE* s, IType left, IType right, IType bound) {
  if (left > right) return -1;

  IType* ija  = IJA<IType>(s);

  if (ija[left] >= bound) return left; // shortcut

  IType mid   = (left + right) / 2;
  IType mid_j = ija[mid];

  if (mid_j == bound)
    return mid;
  else if (mid_j > bound) { // eligible! don't exclude it.
    return binary_search_left_boundary<IType>(s, left, mid, bound);
  } else // (mid_j < bound)
    return binary_search_left_boundary<IType>(s, mid + 1, right, bound);
}


/*
 * Binary search for returning stored values. Returns a non-negative position, or -1 for not found.
 */
template <typename IType>
int binary_search(YALE_STORAGE* s, IType left, IType right, IType key) {
  if (s->src != s) throw; // need to fix this quickly

  if (left > right) return -1;

  IType* ija = reinterpret_cast<IType*>(s->ija);

  IType mid = (left + right)/2;
  IType mid_j = ija[mid];

  if (mid_j == key)
  	return mid;

  else if (mid_j > key)
  	return binary_search<IType>(s, left, mid - 1, key);

  else
  	return binary_search<IType>(s, mid + 1, right, key);
}


/*
 * Resize yale storage vectors A and IJA, copying values.
 */
static void vector_grow(YALE_STORAGE* s) {
  if (s != s->src) {
    throw; // need to correct this quickly.
  }

  size_t new_capacity = s->capacity * GROWTH_CONSTANT;
  size_t max_capacity = max_size(s);

  if (new_capacity > max_capacity) new_capacity = max_capacity;

  void* new_ija       = ALLOC_N(char, ITYPE_SIZES[s->itype] * new_capacity);
  void* new_a         = ALLOC_N(char, DTYPE_SIZES[s->dtype] * new_capacity);

  void* old_ija       = s->ija;
  void* old_a         = s->a;

  memcpy(new_ija, old_ija, s->capacity * ITYPE_SIZES[s->itype]);
  memcpy(new_a,   old_a,   s->capacity * DTYPE_SIZES[s->dtype]);

  s->capacity         = new_capacity;

  xfree(old_ija);
  xfree(old_a);

  s->ija         = new_ija;
  s->a           = new_a;
}


/*
 * Resize yale storage vectors A and IJA in preparation for an insertion.
 */
template <typename DType, typename IType>
static char vector_insert_resize(YALE_STORAGE* s, size_t current_size, size_t pos, size_t* j, size_t n, bool struct_only) {
  if (s != s->src) throw;

  // Determine the new capacity for the IJA and A vectors.
  size_t new_capacity = s->capacity * GROWTH_CONSTANT;
  size_t max_capacity = max_size(s);

  if (new_capacity > max_capacity) {
    new_capacity = max_capacity;

    if (current_size + n > max_capacity) rb_raise(rb_eNoMemError, "insertion size exceeded maximum yale matrix size");
  }

  if (new_capacity < current_size + n)
  	new_capacity = current_size + n;

  // Allocate the new vectors.
  IType* new_ija     = ALLOC_N( IType, new_capacity );
  NM_CHECK_ALLOC(new_ija);

  DType* new_a       = ALLOC_N( DType, new_capacity );
  NM_CHECK_ALLOC(new_a);

  IType* old_ija     = reinterpret_cast<IType*>(s->ija);
  DType* old_a       = reinterpret_cast<DType*>(s->a);

  // Copy all values prior to the insertion site to the new IJA and new A
  if (struct_only) {
    for (size_t i = 0; i < pos; ++i) {
      new_ija[i] = old_ija[i];
    }
  } else {
    for (size_t i = 0; i < pos; ++i) {
      new_ija[i] = old_ija[i];
      new_a[i]   = old_a[i];
    }
  }


  // Copy all values subsequent to the insertion site to the new IJA and new A, leaving room (size n) for insertion.
  if (struct_only) {
    for (size_t i = pos; i < current_size; ++i) {
      new_ija[i+n] = old_ija[i];
    }
  } else {
    for (size_t i = pos; i < current_size; ++i) {
      new_ija[i+n] = old_ija[i];
      new_a[i+n] = old_a[i];
    }
  }

  s->capacity = new_capacity;

  xfree(s->ija);
  xfree(s->a);

  s->ija = reinterpret_cast<void*>(new_ija);
  s->a   = reinterpret_cast<void*>(new_a);

  return 'i';
}

/*
 * Insert a value or contiguous values in the ija and a vectors (after ja and
 * diag). Does not free anything; you are responsible!
 *
 * TODO: Improve this so it can handle non-contiguous element insertions
 *	efficiently. For now, we can just sort the elements in the row in
 *	question.)
 */
template <typename DType, typename IType>
static char vector_insert(YALE_STORAGE* s, size_t pos, size_t* j, void* val_, size_t n, bool struct_only) {

  if (pos < s->shape[0]) {
    rb_raise(rb_eArgError, "vector insert pos (%d) is before beginning of ja (%d); this should not happen", pos, s->shape[0]);
  }

  DType* val = reinterpret_cast<DType*>(val_);

  size_t size = get_size<IType>(s);

  IType* ija = reinterpret_cast<IType*>(s->ija);
  DType* a   = reinterpret_cast<DType*>(s->a);

  if (size + n > s->capacity) {
  	vector_insert_resize<DType,IType>(s, size, pos, j, n, struct_only);

    // Need to get the new locations for ija and a.
  	ija = reinterpret_cast<IType*>(s->ija);
    a   = reinterpret_cast<DType*>(s->a);

  } else {
    /*
     * No resize required:
     * easy (but somewhat slow), just copy elements to the tail, starting at
     * the end, one element at a time.
     *
     * TODO: This can be made slightly more efficient, but only after the tests
     *	are written.
     */

    if (struct_only) {
      for (size_t i = 0; i < size - pos; ++i) {
        ija[size+n-1-i] = ija[size-1-i];
      }
    } else {
      for (size_t i = 0; i < size - pos; ++i) {
        ija[size+n-1-i] = ija[size-1-i];
        a[size+n-1-i]   = a[size-1-i];
      }
    }
  }

  // Now insert the new values.
  if (struct_only) {
    for (size_t i = 0; i < n; ++i) {
      ija[pos+i]  = j[i];
    }
  } else {
    for (size_t i = 0; i < n; ++i) {
      ija[pos+i]  = j[i];
      a[pos+i]    = val[i];
    }
  }

  return 'i';
}

/*
 * If we add n items to row i, we need to increment ija[i+1] and onward.
 */
template <typename IType>
static void increment_ia_after(YALE_STORAGE* s, IType ija_size, IType i, long n) {
  IType* ija = reinterpret_cast<IType*>(s->ija);

  ++i;
  for (; i <= ija_size; ++i) {
    ija[i] += n;
  }
}

/*
 * Binary search for returning insertion points.
 */
template <typename IType>
static IType insert_search(YALE_STORAGE* s, IType left, IType right, IType key, bool* found) {

  if (left > right) {
    *found = false;
    return left;
  }

  IType* ija = reinterpret_cast<IType*>(s->ija);
  IType mid = (left + right)/2;
  IType mid_j = ija[mid];

  if (mid_j == key) {
    *found = true;
    return mid;

  } else if (mid_j > key) {
  	return insert_search<IType>(s, left, mid-1, key, found);

  } else {
  	return insert_search<IType>(s, mid+1, right, key, found);
  }
}

/////////////////////////
// Copying and Casting //
/////////////////////////

/*
 * Templated copy constructor for changing dtypes.
 */
template <typename LDType, typename RDType, typename IType>
YALE_STORAGE* cast_copy(const YALE_STORAGE* rhs, dtype_t new_dtype) {

  YALE_STORAGE* lhs;

  if (rhs->src != rhs) { // copy the reference
    // Copy shape for yale construction
    size_t* shape           = ALLOC_N(size_t, 2);
    shape[0]                = rhs->shape[0];
    shape[1]                = rhs->shape[1];
    size_t ndnz             = src_ndnz(rhs);
    if (shape[0] != rhs->src->shape[0] || shape[1] != rhs->src->shape[1])
      ndnz                  = count_slice_copy_ndnz<RDType,IType>(rhs, rhs->offset, rhs->shape); // expensive, avoid if possible
    size_t request_capacity = shape[0] + ndnz + 1;
    // FIXME: Should we use a different itype? Or same?
    lhs                     = nm_yale_storage_create(new_dtype, shape, 2, request_capacity, rhs->itype);

    // This check probably isn't necessary.
    if (lhs->capacity < request_capacity)
      rb_raise(nm_eStorageTypeError, "conversion failed; capacity of %ld requested, max allowable is %ld", request_capacity, lhs->capacity);

    slice_copy<LDType, RDType, IType>(lhs, rhs, rhs->offset, rhs->shape, new_dtype);
  } else { // regular copy

    // Allocate a new structure
    size_t size = get_size<IType>(rhs);
    lhs = copy_alloc_struct<IType>(rhs, new_dtype, rhs->capacity, size);

    LDType* la = reinterpret_cast<LDType*>(lhs->a);
    RDType* ra = A<RDType>(rhs);

    for (size_t index = 0; index < size; ++index) {
      la[index] = ra[index];
    }
  }

  return lhs;
}

/*
 * Template access for getting the size of Yale storage.
 */
template <typename IType>
static inline size_t get_size(const YALE_STORAGE* storage) {
  return static_cast<size_t>(reinterpret_cast<IType*>(storage->ija)[ storage->shape[0] ]);
}


/*
 * Allocate for a copy or copy-cast operation, and copy the IJA portion of the
 * matrix (the structure).
 */
template <typename IType>
static YALE_STORAGE* copy_alloc_struct(const YALE_STORAGE* rhs, const dtype_t new_dtype, const size_t new_capacity, const size_t new_size) {
  YALE_STORAGE* lhs = ALLOC( YALE_STORAGE );
  lhs->dim          = rhs->dim;
  lhs->shape        = ALLOC_N( size_t, lhs->dim );
  lhs->offset       = ALLOC_N( size_t, lhs->dim );
  memcpy(lhs->shape, rhs->shape, lhs->dim * sizeof(size_t));
  //memcpy(lhs->offset, rhs->offset, lhs->dim * sizeof(size_t));
  lhs->offset[0]    = 0;
  lhs->offset[1]    = 0;

  lhs->itype        = rhs->itype;
  lhs->capacity     = new_capacity;
  lhs->dtype        = new_dtype;
  lhs->ndnz         = rhs->ndnz;

  lhs->ija          = ALLOC_N( IType, lhs->capacity );
  lhs->a            = ALLOC_N( char, DTYPE_SIZES[new_dtype] * lhs->capacity );
  lhs->src          = lhs;
  lhs->count        = 1;

  // Now copy the contents -- but only within the boundaries set by the size. Leave
  // the rest uninitialized.
  if (!rhs->offset[0] && !rhs->offset[1]) {
    for (size_t i = 0; i < get_size<IType>(rhs); ++i)
      reinterpret_cast<IType*>(lhs->ija)[i] = reinterpret_cast<IType*>(rhs->ija)[i]; // copy indices
  } else {
    rb_raise(rb_eNotImpError, "cannot copy struct due to different offsets");
  }
  return lhs;
}

template <typename DType, typename IType>
static STORAGE* matrix_multiply(const STORAGE_PAIR& casted_storage, size_t* resulting_shape, bool vector, nm::itype_t result_itype) {
  YALE_STORAGE *left  = (YALE_STORAGE*)(casted_storage.left),
               *right = (YALE_STORAGE*)(casted_storage.right);

  // We can safely get dtype from the casted matrices; post-condition of binary_storage_cast_alloc is that dtype is the
  // same for left and right.
  // int8_t dtype = left->dtype;

  // Massage the IType arrays into the correct form.

  IType* ijl;
  if (left->itype == result_itype) ijl = reinterpret_cast<IType*>(left->ija);
  else {  // make a temporary copy of the IJA vector for L with the correct itype
    size_t length = nm_yale_storage_get_size(left);
    ijl = ALLOCA_N(IType, length);
    copy_recast_itype_vector(reinterpret_cast<void*>(left->ija), left->itype, reinterpret_cast<void*>(ijl), result_itype, length);
  }

  IType* ijr;
  if (right->itype == result_itype) ijr = reinterpret_cast<IType*>(right->ija);
  else {  // make a temporary copy of the IJA vector for R with the correct itype
    size_t length = nm_yale_storage_get_size(right);
    ijr = ALLOCA_N(IType, length);
    copy_recast_itype_vector(reinterpret_cast<void*>(right->ija), right->itype, reinterpret_cast<void*>(ijr), result_itype, length);
  }

  // First, count the ndnz of the result.
  // TODO: This basically requires running symbmm twice to get the exact ndnz size. That's frustrating. Are there simple
  // cases where we can avoid running it?
  size_t result_ndnz = nm::math::symbmm<IType>(resulting_shape[0], left->shape[1], resulting_shape[1], ijl, ijl, true, ijr, ijr, true, NULL, true);

  // Create result storage.
  YALE_STORAGE* result = nm_yale_storage_create(left->dtype, resulting_shape, 2, result_ndnz, result_itype);
  init<DType,IType>(result, NULL);
  IType* ija = reinterpret_cast<IType*>(result->ija);

  // Symbolic multiplication step (build the structure)
  nm::math::symbmm<IType>(resulting_shape[0], left->shape[1], resulting_shape[1], ijl, ijl, true, ijr, ijr, true, ija, true);

  // Numeric multiplication step (fill in the elements)

  nm::math::numbmm<DType,IType>(result->shape[0], left->shape[1], result->shape[1],
                                ijl, ijl, reinterpret_cast<DType*>(left->a), true,
                                ijr, ijr, reinterpret_cast<DType*>(right->a), true,
                                ija, ija, reinterpret_cast<DType*>(result->a), true);


  // Sort the columns
  nm::math::smmp_sort_columns<DType,IType>(result->shape[0], ija, ija, reinterpret_cast<DType*>(result->a));

  return reinterpret_cast<STORAGE*>(result);
}


/*
 * Get the sum of offsets from the original matrix (for sliced iteration).
 */
static std::array<size_t,2> get_offsets(YALE_STORAGE* x) {
  std::array<size_t, 2> offsets{ {0,0} };
  while (x != x->src) {
    offsets[0] += x->offset[0];
    offsets[1] += x->offset[1];
    x = reinterpret_cast<YALE_STORAGE*>(x->src);
  }
  return offsets;
}


template <typename IType>
class IJAManager {
protected:
  bool needs_free;

public:
  IType* ija;

  IJAManager(YALE_STORAGE* s, itype_t temp_itype) : needs_free(false), ija(IJA<IType>(s)) {
    if (s->itype != temp_itype) {
      size_t len  = nm_yale_storage_get_size(s);
      needs_free  = true;
      ija         = ALLOC_N(IType, len);
      copy_recast_itype_vector(IJA<IType>(s), s->itype, reinterpret_cast<void*>(ija), temp_itype, len);
    }
  }

  ~IJAManager() {
    if (needs_free) xfree(ija);
  }
};


template <typename IType>
class RowIterator {
protected:
  YALE_STORAGE* s;
  IType* ija;
  void*  a;
  IType i, k, k_end;
  size_t j_offset, j_shape;
  bool diag, End;
  VALUE init;
public:
  RowIterator(YALE_STORAGE* s_, IType* ija_, IType i_, size_t j_shape_, size_t j_offset_ = 0)
    : s(s_),
      ija(ija_),
      a(reinterpret_cast<YALE_STORAGE*>(s->src)->a),
      i(i_),
      k(ija[i]),
      k_end(ija[i+1]),
      j_offset(j_offset_),
      j_shape(j_shape_),
      diag(row_has_no_nd() || diag_is_first()),
      End(false),
      init(default_value(s))
    { }

  RowIterator(YALE_STORAGE* s_, IType i_, size_t j_shape_, size_t j_offset_ = 0)
    : s(s_),
      ija(IJA<IType>(s)),
      a(reinterpret_cast<YALE_STORAGE*>(s->src)->a),
      i(i_),
      k(ija[i]),
      k_end(ija[i+1]),
      j_offset(j_offset_),
      j_shape(j_shape_),
      diag(row_has_no_nd() || diag_is_first()),
      End(false),
      init(default_value(s))
  { }

  RowIterator(const RowIterator& rhs) : s(rhs.s), ija(rhs.ija), a(s->a), i(rhs.i), k(rhs.k), k_end(rhs.k_end), j_offset(rhs.j_offset), j_shape(rhs.j_shape), diag(rhs.diag), End(rhs.End), init(rhs.init) { }

  VALUE obj() const {
    return diag ? obj_at(s, i) : obj_at(s, k);
  }

  template <typename T>
  T cobj() const {
    if (typeid(T) == typeid(RubyObject)) return obj();
    return A<T>(s)[diag ? i : k];
  }

  inline IType proper_j() const {
    return diag ? i : ija[k];
  }

  inline IType offset_j() const {
    return proper_j() - j_offset;
  }

  /* Returns true if an additional value is inserted, false if it goes on the diagonal */
  bool insert(IType j, VALUE v) {
    if (j == i) { // insert regardless on diagonal
      reinterpret_cast<VALUE*>(a)[j] = v;
      return false;

    } else {
      if (rb_funcall(v, rb_intern("!="), 1, init) == Qtrue) {
        if (k >= s->capacity) {
          vector_grow(s);
          ija = IJA<IType>(s);
          a   = reinterpret_cast<YALE_STORAGE*>(s->src)->a;
        }
        reinterpret_cast<VALUE*>(a)[k] = v;
        ija[k] = j;
        k++;
        return true;
      }
      return false;
    }
  }

  void update_row_end() {
    ija[i+1] = k;
    k_end    = k;
  }

  /* Past the j_shape? */
  inline bool end() const {
    if (End)  return true;
    //if (diag) return i - j_offset >= j_shape;
    //else return k >= s->capacity || ija[k] - j_offset >= j_shape;
    return (diag ? i : ija[k]) - j_offset >= j_shape;
  }

  inline bool row_has_no_nd() const { return ija[i] == k_end; /* k_start == k_end */  }
  inline bool diag_is_first() const { return i < ija[ija[i]];  }
  inline bool diag_is_last() const  { return i > ija[k_end-1]; } // only works if !row_has_no_nd()
  inline bool k_is_last_nd() const  { return k == k_end-1;     }
  inline bool k_is_last() const     { return k_is_last_nd() && !diag_is_last(); }
  inline bool diag_is_ahead() const { return i > ija[k]; }
  inline bool row_has_diag() const  { return i < s->shape[1];  }
  inline bool diag_is_next() const  { // assumes we've already tested for diag, row_has_no_nd(), diag_is_first()
    if (i == ija[k]+1) return true; // definite next
    else if (k+1 < k_end && i >= ija[k+1]+1) return false; // at least one item before it
    else return true;
  }

  RowIterator<IType>& operator++() {
    if (diag) {                                             // we're at the diagonal
      if (row_has_no_nd() || diag_is_last()) End = true;    //  and there are no non-diagonals (or none still to visit)
      diag = false;
    } else if (!row_has_diag()) {                           // row has no diagonal entries
      if (row_has_no_nd() || k_is_last_nd()) End = true;    // row is totally empty, or we're at last entry
      else k++;                                             // still entries to visit
    } else { // not at diag but it exists somewhere in the row, and row has at least one nd entry
      if (diag_is_ahead()) { // diag is ahead
        if (k_is_last_nd()) diag = true; // diag is next and last
        else if (diag_is_next()) {       // diag is next and not last
          diag = true;
          k++;
        } else k++;                      // diag is not next
      } else {                           // diag is past
        if (k_is_last_nd()) End = true;  //   and we're at the end
        else k++;                        //   and we're not at the end
      }
    }

    return *this;
  }


  RowIterator<IType> operator++(int unused) {
    RowIterator<IType> x(*this);
    ++(*this);
    return x;
  }
};



template <typename IType>
static VALUE map_stored(VALUE self) {

  YALE_STORAGE* s = NM_STORAGE_YALE(self);

  size_t* shape   = ALLOC_N(size_t, 2);
  shape[0]        = s->shape[0];
  shape[1]        = s->shape[1];

  std::array<size_t,2>  s_offsets = get_offsets(s);

  RETURN_SIZED_ENUMERATOR(self, 0, 0, nm_yale_enumerator_length);
  VALUE init      = rb_yield(default_value(s));

  // Try to find a reasonable capacity to request when creating the matrix
  size_t ndnz     = src_ndnz(s);
  if (s->src != s) // need to guess capacity
    ndnz = yale_count_slice_copy_ndnz(s, s->offset, s->shape);
  size_t request_capacity = s->shape[0] + ndnz + 1;

  YALE_STORAGE* r = nm_yale_storage_create(nm::RUBYOBJ, shape, 2, request_capacity, NM_ITYPE(self));
  if (r->capacity < request_capacity)
    rb_raise(nm_eStorageTypeError, "conversion failed; capacity of %ld requested, max allowable is %ld", request_capacity, r->capacity);
  nm_yale_storage_init(r, &init);

  for (IType ri = 0; ri < shape[0]; ++ri) {
    RowIterator<IType> sit(s, ri + s_offsets[0], shape[1], s_offsets[1]);
    RowIterator<IType> rit(r, ri, shape[1]);

    while (!sit.end()) {
      VALUE rv = rb_yield(sit.obj());
      VALUE rj = sit.offset_j();
      rit.insert(rj, rv);
      ++sit;
    }
    // Update the row end information.
    rit.update_row_end();
  }

  NMATRIX* m = nm_create(nm::YALE_STORE, reinterpret_cast<STORAGE*>(r));
  return Data_Wrap_Struct(CLASS_OF(self), nm_yale_storage_mark, nm_delete, m);
}


/*
 * eqeq function for slicing and different defaults.
 */
template <typename LDType, typename RDType, typename IType>
static bool eqeq_different_defaults(const YALE_STORAGE* s, const LDType& s_init, const YALE_STORAGE* t, const RDType& t_init) {

  std::array<size_t,2>  s_offsets = get_offsets(const_cast<YALE_STORAGE*>(s)),
                        t_offsets = get_offsets(const_cast<YALE_STORAGE*>(t));

  for (IType ri = 0; ri < s->shape[0]; ++ri) {
    RowIterator<IType> sit(const_cast<YALE_STORAGE*>(s), IJA<IType>(s), ri + s_offsets[0], s->shape[1], s_offsets[1]);
    RowIterator<IType> tit(const_cast<YALE_STORAGE*>(t), IJA<IType>(t), ri + t_offsets[0], s->shape[1], t_offsets[1]);

    while (!sit.end() || !tit.end()) {

      // Perform the computation. Use a default value if the matrix doesn't have some value stored.
      if (tit.end() || (!sit.end() && sit.offset_j() < tit.offset_j())) {
        if (sit.template cobj<LDType>() != t_init) return false;
        ++sit;

      } else if (sit.end() || (!tit.end() && sit.offset_j() > tit.offset_j())) {
        if (s_init != tit.template cobj<RDType>()) return false;
        ++tit;

      } else {  // same index
        if (sit.template cobj<LDType>() != tit.template cobj<RDType>()) return false;
        ++sit;
        ++tit;
      }
    }
  }
  return true;
}


template <typename IType>
static VALUE map_merged_stored(VALUE left, VALUE right, VALUE init, nm::itype_t itype) {

  YALE_STORAGE *s = NM_STORAGE_YALE(left),
               *t = NM_STORAGE_YALE(right);

  size_t* shape   = ALLOC_N(size_t, 2);
  shape[0]        = s->shape[0];
  shape[1]        = s->shape[1];

  std::array<size_t,2>  s_offsets = get_offsets(s),
                        t_offsets = get_offsets(t);

  VALUE s_init    = default_value(s),
        t_init    = default_value(t);

  RETURN_SIZED_ENUMERATOR(left, 0, 0, 0);

  if (init == Qnil)
    init          = rb_yield_values(2, s_init, t_init);

  // Make a reasonable approximation of the resulting capacity
  size_t s_ndnz = src_ndnz(s), t_ndnz = src_ndnz(t);
  if (s->src != s) s_ndnz = yale_count_slice_copy_ndnz(s, s->offset, s->shape);
  if (t->src != t) t_ndnz = yale_count_slice_copy_ndnz(t, t->offset, t->shape);
  size_t request_capacity = shape[0] + NM_MAX(s_ndnz, t_ndnz) + 1;

  YALE_STORAGE* r = nm_yale_storage_create(nm::RUBYOBJ, shape, 2, request_capacity, itype);
  if (r->capacity < request_capacity)
    rb_raise(nm_eStorageTypeError, "conversion failed; capacity of %ld requested, max allowable is %ld", request_capacity, r->capacity);

  nm_yale_storage_init(r, &init);

  IJAManager<IType> sm(s, itype),
                    tm(t, itype);

  for (IType ri = 0; ri < shape[0]; ++ri) {
    RowIterator<IType> sit(s, sm.ija, ri + s_offsets[0], shape[1], s_offsets[1]);
    RowIterator<IType> tit(t, tm.ija, ri + t_offsets[0], shape[1], t_offsets[1]);

    RowIterator<IType> rit(r, reinterpret_cast<IType*>(r->ija), ri, shape[1]);
    while (!sit.end() || !tit.end()) {
      VALUE rv;
      IType rj;

      // Perform the computation. Use a default value if the matrix doesn't have some value stored.
      if (tit.end() || (!sit.end() && sit.offset_j() < tit.offset_j())) {
        rv = rb_yield_values(2, sit.obj(), t_init);
        rj = sit.offset_j();
        ++sit;

      } else if (sit.end() || (!tit.end() && sit.offset_j() > tit.offset_j())) {
        rv = rb_yield_values(2, s_init, tit.obj());
        rj = tit.offset_j();
        ++tit;

      } else {  // same index
        rv = rb_yield_values(2, sit.obj(), tit.obj());
        rj = sit.offset_j();
        ++sit;
        ++tit;
      }

      rit.insert(rj, rv); // handles increment (and testing for default, etc)

    }

    // Update the row end information.
    rit.update_row_end();
  }

  NMATRIX* m = nm_create(nm::YALE_STORE, reinterpret_cast<STORAGE*>(r));
  return Data_Wrap_Struct(CLASS_OF(left), nm_yale_storage_mark, nm_delete, m);
}


/*
 * This function and the two helper structs enable us to use partial template specialization.
 * See also: http://stackoverflow.com/questions/6623375/c-template-specialization-on-functions
 */
template <typename DType, typename IType>
static VALUE each_stored_with_indices(VALUE nm) {
  YALE_STORAGE* s = NM_STORAGE_YALE(nm);
  DType* a        = A<DType>(s);
  IType* ija      = IJA<IType>(s);

  // If we don't have a block, return an enumerator.
  RETURN_SIZED_ENUMERATOR(nm, 0, 0, nm_yale_stored_enumerator_length);

  // Iterate along diagonal
  for (size_t sk = NM_MAX(s->offset[0], s->offset[1]); sk < NM_MIN(s->shape[0] + s->offset[0], s->shape[1] + s->offset[1]); ++sk) {
    VALUE ii = LONG2NUM(sk - s->offset[0]),
          jj = LONG2NUM(sk - s->offset[1]);

    rb_yield_values(3, obj_at(s, sk), ii, jj);
  }

  // Iterate through non-diagonal elements, row by row
  for (long ri = 0; ri < s->shape[0]; ++ri) {
    long si      = ri + s->offset[0];
    IType p      = ija[si],
          next_p = ija[si+1];

    // if this is a reference to another matrix, we should find the left boundary of the slice
    if (s != s->src && p < next_p)
      p = binary_search_left_boundary<IType>(s, p, next_p-1, s->offset[1]);

    for (; p < next_p; ++p) {
      long sj = static_cast<long>(ija[p]),
           rj = sj - s->offset[1];
      if (rj < 0) continue;

      if (rj >= s->shape[1]) break;

      rb_yield_values(3, obj_at(s, p), LONG2NUM(ri), LONG2NUM(rj));
    }
  }

  return nm;
}

template <typename DType, typename IType>
static VALUE each_with_indices(VALUE nm) {
  YALE_STORAGE* s = NM_STORAGE_YALE(nm);
  DType* a        = A<DType>(s);
  IType* ija      = IJA<IType>(s);

  // If we don't have a block, return an enumerator.
  RETURN_SIZED_ENUMERATOR(nm, 0, 0, nm_yale_enumerator_length);

  // Iterate in two dimensions.
  // s stands for src, r stands for ref (for ri, rj, si, sj)
  for (long ri = 0; ri < s->shape[0]; ++ri) {
    long si  = ri + s->offset[0];
    VALUE ii = LONG2NUM(ri + s->offset[0]);

    IType k = ija[si], k_next = ija[si+1];

    for (long rj = 0; rj < s->shape[1]; ++rj) {
      long sj     = rj + s->offset[1];
      VALUE v, jj = LONG2NUM(rj);

      // zero is stored in s->shape[0]
      if (si == sj) {
        v = obj_at(s, si);
      } else {
        // Walk through the row until we find the correct location.
        while (ija[k] < sj && k < k_next) ++k;
        if (k < k_next && ija[k] == sj) {
          v = obj_at(s, k);
          ++k;
        } else v = default_value(s); // rubyobj_from_cval(&(a[s->shape[0]]), NM_DTYPE(nm)).rval;
      }
      rb_yield_values(3, v, ii, jj);
    }
  }

  return nm;
}


} // end of namespace nm::yale_storage


// Helper function used only for the RETURN_SIZED_ENUMERATOR macro. Returns the length of
// the matrix's storage.
static VALUE nm_yale_stored_enumerator_length(VALUE nmatrix) {
  long len = nm_yale_storage_get_size(NM_STORAGE_YALE(nmatrix));
  return LONG2NUM(len);
}


} // end of namespace nm.

///////////////////
// Ruby Bindings //
///////////////////

/* These bindings are mostly only for debugging Yale. They are called from Init_nmatrix. */

extern "C" {

void nm_init_yale_functions() {
	/*
	 * This module stores methods that are useful for debugging Yale matrices,
	 * i.e. the ones with +:yale+ stype.	
	 */
  cNMatrix_YaleFunctions = rb_define_module_under(cNMatrix, "YaleFunctions");

  rb_define_method(cNMatrix_YaleFunctions, "yale_ija", (METHOD)nm_ija, -1);
  rb_define_method(cNMatrix_YaleFunctions, "yale_a", (METHOD)nm_a, -1);
  rb_define_method(cNMatrix_YaleFunctions, "yale_size", (METHOD)nm_size, 0);
  rb_define_method(cNMatrix_YaleFunctions, "yale_ia", (METHOD)nm_ia, 0);
  rb_define_method(cNMatrix_YaleFunctions, "yale_ja", (METHOD)nm_ja, 0);
  rb_define_method(cNMatrix_YaleFunctions, "yale_d", (METHOD)nm_d, -1);
  rb_define_method(cNMatrix_YaleFunctions, "yale_lu", (METHOD)nm_lu, 0);

  rb_define_method(cNMatrix_YaleFunctions, "yale_nd_row", (METHOD)nm_nd_row, -1);

  rb_define_const(cNMatrix_YaleFunctions, "YALE_GROWTH_CONSTANT", rb_float_new(nm::yale_storage::GROWTH_CONSTANT));
}


/////////////////
// C ACCESSORS //
/////////////////


/* C interface for NMatrix#each_with_indices (Yale) */
VALUE nm_yale_each_with_indices(VALUE nmatrix) {
  nm::dtype_t d = NM_DTYPE(nmatrix);
  nm::itype_t i = NM_ITYPE(nmatrix);

  NAMED_LI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::each_with_indices, VALUE, VALUE)

  return ttable[d][i](nmatrix);
}


/* C interface for NMatrix#each_stored_with_indices (Yale) */
VALUE nm_yale_each_stored_with_indices(VALUE nmatrix) {
  nm::dtype_t d = NM_DTYPE(nmatrix);
  nm::itype_t i = NM_ITYPE(nmatrix);

  NAMED_LI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::each_stored_with_indices, VALUE, VALUE)

  return ttable[d][i](nmatrix);
}



/*
 * C accessor for inserting some value in a matrix (or replacing an existing cell).
 */
void nm_yale_storage_set(VALUE left, SLICE* slice, VALUE right) {
  NAMED_LI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::set, void, VALUE left, SLICE* slice, VALUE right);

  YALE_STORAGE* storage = NM_STORAGE_YALE(left);

  ttable[storage->dtype][storage->itype](left, slice, right);
}


/*
 * Determine the number of non-diagonal non-zeros in a not-yet-created copy of a slice or matrix.
 */
static size_t yale_count_slice_copy_ndnz(const YALE_STORAGE* s, size_t* offset, size_t* shape) {
  NAMED_LI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::count_slice_copy_ndnz, size_t, const YALE_STORAGE*, size_t*, size_t*)

  return ttable[s->dtype][s->itype](s, offset, shape);
}


/*
 * C accessor for yale_storage::get, which returns a slice of YALE_STORAGE object by copy
 *
 * Slicing-related.
 */
void* nm_yale_storage_get(STORAGE* storage, SLICE* slice) {
  YALE_STORAGE* casted_storage = (YALE_STORAGE*)storage;

  if (slice->single) {
    NAMED_LI_DTYPE_TEMPLATE_TABLE(elem_copy_table,  nm::yale_storage::get_single, void*, YALE_STORAGE*, SLICE*)

    return elem_copy_table[casted_storage->dtype][casted_storage->itype](casted_storage, slice);
  } else {
    // Copy shape for yale construction
    size_t* shape           = ALLOC_N(size_t, 2);
    shape[0]                = slice->lengths[0];
    shape[1]                = slice->lengths[1];

    // only count ndnz if our slice is smaller, otherwise use the given value
    size_t ndnz             = src_ndnz(casted_storage);
    if (shape[0] != casted_storage->shape[0] || shape[1] != casted_storage->shape[1])
      ndnz = yale_count_slice_copy_ndnz(casted_storage, slice->coords, shape); // expensive operation

    size_t request_capacity = shape[0] + ndnz + 1; // capacity of new matrix
    YALE_STORAGE* ns        = nm_yale_storage_create(casted_storage->dtype, shape, 2, request_capacity, casted_storage->itype);

    // This check probably isn't necessary.
    if (ns->capacity < request_capacity)
      rb_raise(nm_eStorageTypeError, "conversion failed; capacity of %ld requested, max allowable is %ld", request_capacity, ns->capacity);

    NAMED_LRI_DTYPE_TEMPLATE_TABLE(slice_copy_table, nm::yale_storage::slice_copy, void, YALE_STORAGE* ns, const YALE_STORAGE* s, size_t*, size_t*, nm::dtype_t)

    slice_copy_table[ns->dtype][casted_storage->dtype][casted_storage->itype](ns, casted_storage, slice->coords, slice->lengths, casted_storage->dtype);

    return ns;
  }
}

/*
 * C accessor for yale_storage::vector_insert
 */
static char nm_yale_storage_vector_insert(YALE_STORAGE* s, size_t pos, size_t* js, void* vals, size_t n, bool struct_only, nm::dtype_t dtype, nm::itype_t itype) {
  NAMED_LI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::vector_insert, char, YALE_STORAGE*, size_t, size_t*, void*, size_t, bool);

  return ttable[dtype][itype](s, pos, js, vals, n, struct_only);
}

/*
 * C accessor for yale_storage::increment_ia_after, typically called after ::vector_insert
 */
static void nm_yale_storage_increment_ia_after(YALE_STORAGE* s, size_t ija_size, size_t i, long n, nm::itype_t itype) {
  NAMED_ITYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::c_increment_ia_after, void, YALE_STORAGE*, size_t, size_t, long);

  ttable[itype](s, ija_size, i, n);
}


/*
 * C accessor for yale_storage::ref, which returns a pointer to the correct location in a YALE_STORAGE object
 * for some set of coordinates.
 */
void* nm_yale_storage_ref(STORAGE* storage, SLICE* slice) {
  YALE_STORAGE* casted_storage = (YALE_STORAGE*)storage;

  if (slice->single) {
    NAMED_LI_DTYPE_TEMPLATE_TABLE(elem_copy_table,  nm::yale_storage::get_single, void*, YALE_STORAGE*, SLICE*)
    return elem_copy_table[casted_storage->dtype][casted_storage->itype](casted_storage, slice);
  } else {
    NAMED_LI_DTYPE_TEMPLATE_TABLE(ref_table, nm::yale_storage::ref, void*, YALE_STORAGE* storage, SLICE* slice)
    return ref_table[casted_storage->dtype][casted_storage->itype](casted_storage, slice);
  }
}


/*
 * C accessor for determining whether two YALE_STORAGE objects have the same contents.
 */
bool nm_yale_storage_eqeq(const STORAGE* left, const STORAGE* right) {
  NAMED_LRI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::eqeq, bool, const YALE_STORAGE* left, const YALE_STORAGE* right);

  const YALE_STORAGE* casted_left = reinterpret_cast<const YALE_STORAGE*>(left);

  return ttable[casted_left->dtype][right->dtype][casted_left->itype](casted_left, (const YALE_STORAGE*)right);
}


/*
 * Copy constructor for changing dtypes. (C accessor)
 */
STORAGE* nm_yale_storage_cast_copy(const STORAGE* rhs, nm::dtype_t new_dtype, void* dummy) {
  NAMED_LRI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::cast_copy, YALE_STORAGE*, const YALE_STORAGE* rhs, nm::dtype_t new_dtype);

  const YALE_STORAGE* casted_rhs = reinterpret_cast<const YALE_STORAGE*>(rhs);

  return (STORAGE*)ttable[new_dtype][casted_rhs->dtype][casted_rhs->itype](casted_rhs, new_dtype);
}


/*
 * Returns size of Yale storage as a size_t (no matter what the itype is). (C accessor)
 */
size_t nm_yale_storage_get_size(const YALE_STORAGE* storage) {
  NAMED_ITYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::get_size, size_t, const YALE_STORAGE* storage);

  return ttable[storage->itype](storage);
}



/*
 * Return a void pointer to the matrix's default value entry.
 */
static void* default_value_ptr(const YALE_STORAGE* s) {
  return reinterpret_cast<void*>(reinterpret_cast<char*>(((YALE_STORAGE*)(s->src))->a) + (((YALE_STORAGE*)(s->src))->shape[0] * DTYPE_SIZES[s->dtype]));
}

/*
 * Return the Ruby object at a given location in storage.
 */
static VALUE obj_at(YALE_STORAGE* s, size_t k) {
  if (s->dtype == nm::RUBYOBJ)  return reinterpret_cast<VALUE*>(((YALE_STORAGE*)(s->src))->a)[k];
  else  return rubyobj_from_cval(reinterpret_cast<void*>(reinterpret_cast<char*>(((YALE_STORAGE*)(s->src))->a) + k * DTYPE_SIZES[s->dtype]), s->dtype).rval;
}


/*
 * Return the matrix's default value as a Ruby VALUE.
 */
static VALUE default_value(const YALE_STORAGE* s) {
  if (s->dtype == nm::RUBYOBJ) return *reinterpret_cast<VALUE*>(default_value_ptr(s));
  else return rubyobj_from_cval(default_value_ptr(s), s->dtype).rval;
}


/*
 * Check to see if a default value is some form of zero. Easy for non-Ruby object matrices, which should always be 0.
 */
static bool default_value_is_numeric_zero(const YALE_STORAGE* s) {
  return rb_funcall(default_value(s), rb_intern("=="), 1, INT2FIX(0)) == Qtrue;
}


/*
 * C accessor for allocating a yale storage object for cast-copying. Copies the IJA vector, does not copy the A vector.
 */
static YALE_STORAGE* nm_copy_alloc_struct(const YALE_STORAGE* rhs, const nm::dtype_t new_dtype, const size_t new_capacity, const size_t new_size) {
  NAMED_ITYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::copy_alloc_struct, YALE_STORAGE*, const YALE_STORAGE* rhs, const nm::dtype_t new_dtype, const size_t new_capacity, const size_t new_size);

  return ttable[rhs->itype](rhs, new_dtype, new_capacity, new_size);
}

/*
 * Transposing copy constructor.
 */
STORAGE* nm_yale_storage_copy_transposed(const STORAGE* rhs_base) {
  YALE_STORAGE* rhs = (YALE_STORAGE*)rhs_base;

  if (rhs->src != rhs)
    rb_raise(rb_eNotImpError, "must be called on a real matrix and not a slice");

  size_t* shape = ALLOC_N(size_t, 2);
  shape[0] = rhs->shape[1];
  shape[1] = rhs->shape[0];

  size_t size   = nm_yale_storage_get_size(rhs);

  YALE_STORAGE* lhs = nm_yale_storage_create(rhs->dtype, shape, 2, size, rhs->itype);
  nm_yale_storage_init(lhs, default_value_ptr(rhs));

  NAMED_LI_DTYPE_TEMPLATE_TABLE(transp, nm::math::transpose_yale, void, const size_t n, const size_t m, const void* ia_, const void* ja_, const void* a_, const bool diaga, void* ib_, void* jb_, void* b_, const bool move);

  transp[lhs->dtype][lhs->itype](rhs->shape[0], rhs->shape[1], rhs->ija, rhs->ija, rhs->a, true, lhs->ija, lhs->ija, lhs->a, true);

  return (STORAGE*)lhs;
}

/*
 * C accessor for multiplying two YALE_STORAGE matrices, which have already been casted to the same dtype.
 *
 * FIXME: There should be some mathematical way to determine the worst-case IType based on the input ITypes. Right now
 * it just uses the default.
 */
STORAGE* nm_yale_storage_matrix_multiply(const STORAGE_PAIR& casted_storage, size_t* resulting_shape, bool vector) {
  LI_DTYPE_TEMPLATE_TABLE(nm::yale_storage::matrix_multiply, STORAGE*, const STORAGE_PAIR& casted_storage, size_t* resulting_shape, bool vector, nm::itype_t resulting_itype);

  YALE_STORAGE* left = reinterpret_cast<YALE_STORAGE*>(casted_storage.left);
  YALE_STORAGE* right = reinterpret_cast<YALE_STORAGE*>(casted_storage.right);

  if (!default_value_is_numeric_zero(left) || !default_value_is_numeric_zero(right)) {
    rb_raise(rb_eNotImpError, "matrix default value must be some form of zero (not false or nil) for multiplication");
    return NULL;
  }

  // Determine the itype for the matrix that will be returned.
  nm::itype_t itype = nm_yale_storage_itype_by_shape(resulting_shape),
              max_itype = NM_MAX_ITYPE(left->itype, right->itype);
  if (static_cast<int8_t>(itype) < static_cast<int8_t>(max_itype)) itype = max_itype;

  return ttable[left->dtype][itype](casted_storage, resulting_shape, vector, itype);
}


///////////////
// Lifecycle //
///////////////

/*
 * C accessor function for creating a YALE_STORAGE object. Prior to calling this function, you MUST
 * allocate shape (should be size_t * 2) -- don't use use a regular size_t array!
 *
 * For this type, dim must always be 2. The final argument is the initial capacity with which to
 * create the storage.
 */

YALE_STORAGE* nm_yale_storage_create(nm::dtype_t dtype, size_t* shape, size_t dim, size_t init_capacity, nm::itype_t min_itype) {
  YALE_STORAGE* s;
  size_t max_capacity;

	// FIXME: This error should be handled in the nmatrix.c file.
  if (dim != 2) {
   	rb_raise(rb_eNotImpError, "Can only support 2D matrices");
  }

  s = alloc(dtype, shape, dim, min_itype);
  max_capacity = nm::yale_storage::max_size(s);

  // Set matrix capacity (and ensure its validity)
  if (init_capacity < NM_YALE_MINIMUM(s)) {
  	s->capacity = NM_YALE_MINIMUM(s);

  } else if (init_capacity > max_capacity) {
  	// Don't allow storage to be created larger than necessary
  	s->capacity = max_capacity;

  } else {
  	s->capacity = init_capacity;

  }

  s->ija = ALLOC_N( char, ITYPE_SIZES[s->itype] * s->capacity );
  s->a   = ALLOC_N( char, DTYPE_SIZES[s->dtype] * s->capacity );

  return s;
}

/*
 * Destructor for yale storage (C-accessible).
 */
void nm_yale_storage_delete(STORAGE* s) {
  if (s) {
    YALE_STORAGE* storage = (YALE_STORAGE*)s;
    if (storage->count-- == 1) {
      xfree(storage->shape);
      xfree(storage->offset);
      xfree(storage->ija);
      xfree(storage->a);
      xfree(storage);
    }
  }
}

/*
 * Destructor for the yale storage ref
 */
void nm_yale_storage_delete_ref(STORAGE* s) {
  if (s) {
    YALE_STORAGE* storage = (YALE_STORAGE*)s;
    nm_yale_storage_delete( reinterpret_cast<STORAGE*>(storage->src) );
    xfree(storage->shape);
    xfree(storage->offset);
    xfree(s);
  }
}

/*
 * C accessor for yale_storage::init, a templated function.
 *
 * Initializes the IJA vector of the YALE_STORAGE matrix.
 */
void nm_yale_storage_init(YALE_STORAGE* s, void* init_val) {
  NAMED_LI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::init, void, YALE_STORAGE*, void*);

  ttable[s->dtype][s->itype](s, init_val);
}


/*
 * Ruby GC mark function for YALE_STORAGE. C accessible.
 */
void nm_yale_storage_mark(void* storage_base) {
  YALE_STORAGE* storage = (YALE_STORAGE*)storage_base;
  size_t i;

  if (storage && storage->dtype == nm::RUBYOBJ) {
  	for (i = storage->capacity; i-- > 0;) {
      rb_gc_mark(*((VALUE*)((char*)(storage->a) + i*DTYPE_SIZES[nm::RUBYOBJ])));
    }
  }
}


/*
 * Allocates and initializes the basic struct (but not the IJA or A vectors).
 */
static YALE_STORAGE* alloc(nm::dtype_t dtype, size_t* shape, size_t dim, nm::itype_t min_itype) {
  YALE_STORAGE* s;

  s = ALLOC( YALE_STORAGE );

  s->ndnz        = 0;
  s->dtype       = dtype;
  s->shape       = shape;
  s->offset      = ALLOC_N(size_t, dim);
  for (size_t i = 0; i < dim; ++i)
    s->offset[i] = 0;
  s->dim         = dim;
  s->itype       = nm_yale_storage_itype_by_shape(shape);
  s->src         = reinterpret_cast<STORAGE*>(s);
  s->count       = 1;

  // See if a higher itype has been requested.
  if (static_cast<int8_t>(s->itype) < static_cast<int8_t>(min_itype))
    s->itype    = min_itype;

  return s;
}

YALE_STORAGE* nm_yale_storage_create_from_old_yale(nm::dtype_t dtype, size_t* shape, void* ia, void* ja, void* a, nm::dtype_t from_dtype) {

  NAMED_LRI_DTYPE_TEMPLATE_TABLE(ttable, nm::yale_storage::create_from_old_yale, YALE_STORAGE*, nm::dtype_t dtype, size_t* shape, void* r_ia, void* r_ja, void* r_a);

  // With C++ templates, we don't want to have a 4-parameter template. That would be LDType, RDType, LIType, RIType.
  // We can prevent that by copying ia and ja into the correct itype (if necessary) before passing them to the yale
  // copy constructor.
  nm::itype_t to_itype = nm_yale_storage_itype_by_shape(shape);

  return ttable[dtype][from_dtype][to_itype](dtype, shape, ia, ja, a);

}

//////////////////////////////////////////////
// YALE-SPECIFIC FUNCTIONS (RUBY ACCESSORS) //
//////////////////////////////////////////////

/*
 * call-seq:
 *     yale_size -> Integer
 *
 * Get the size of a Yale matrix (the number of elements actually stored).
 *
 * For capacity (the maximum number of elements that can be stored without a resize), use capacity instead.
 */
static VALUE nm_size(VALUE self) {
  YALE_STORAGE* s = (YALE_STORAGE*)(NM_SRC(self));

  return rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[s->itype]*(s->shape[0]), s->itype).rval;
}


/*
 * call-seq:
 *     yale_a -> Array
 *     yale_d(index) -> ...
 *
 * Get the A array of a Yale matrix (which stores the diagonal and the LU portions of the matrix).
 */
static VALUE nm_a(int argc, VALUE* argv, VALUE self) {
  VALUE idx;
  rb_scan_args(argc, argv, "01", &idx);

  YALE_STORAGE* s = reinterpret_cast<YALE_STORAGE*>(NM_SRC(self));
  size_t size = nm_yale_storage_get_size(s);

  if (idx == Qnil) {
    VALUE* vals = ALLOCA_N(VALUE, size);

    if (NM_DTYPE(self) == nm::RUBYOBJ) {
      for (size_t i = 0; i < size; ++i) {
        vals[i] = reinterpret_cast<VALUE*>(s->a)[i];
      }
    } else {
      for (size_t i = 0; i < size; ++i) {
        vals[i] = rubyobj_from_cval((char*)(s->a) + DTYPE_SIZES[s->dtype]*i, s->dtype).rval;
      }
    }
    VALUE ary = rb_ary_new4(size, vals);

    for (size_t i = size; i < s->capacity; ++i)
      rb_ary_push(ary, Qnil);

    return ary;
  } else {
    size_t index = FIX2INT(idx);
    if (index >= size) rb_raise(rb_eRangeError, "out of range");

    return rubyobj_from_cval((char*)(s->a) + DTYPE_SIZES[s->dtype] * index, s->dtype).rval;
  }
}


/*
 * call-seq:
 *     yale_d -> Array
 *     yale_d(index) -> ...
 *
 * Get the diagonal ("D") portion of the A array of a Yale matrix.
 */
static VALUE nm_d(int argc, VALUE* argv, VALUE self) {
  VALUE idx;
  rb_scan_args(argc, argv, "01", &idx);

  YALE_STORAGE* s = reinterpret_cast<YALE_STORAGE*>(NM_SRC(self));

  if (idx == Qnil) {
    VALUE* vals = ALLOCA_N(VALUE, s->shape[0]);

    if (NM_DTYPE(self) == nm::RUBYOBJ) {
      for (size_t i = 0; i < s->shape[0]; ++i) {
        vals[i] = reinterpret_cast<VALUE*>(s->a)[i];
      }
    } else {
      for (size_t i = 0; i < s->shape[0]; ++i) {
        vals[i] = rubyobj_from_cval((char*)(s->a) + DTYPE_SIZES[s->dtype]*i, s->dtype).rval;
      }
    }

    return rb_ary_new4(s->shape[0], vals);
  } else {
    size_t index = FIX2INT(idx);
    if (index >= s->shape[0]) rb_raise(rb_eRangeError, "out of range");

    return rubyobj_from_cval((char*)(s->a) + DTYPE_SIZES[s->dtype] * index, s->dtype).rval;
  }
}

/*
 * call-seq:
 *     yale_lu -> Array
 *
 * Get the non-diagonal ("LU") portion of the A array of a Yale matrix.
 */
static VALUE nm_lu(VALUE self) {
  YALE_STORAGE* s = reinterpret_cast<YALE_STORAGE*>(NM_SRC(self));

  size_t size = nm_yale_storage_get_size(s);

  VALUE* vals = ALLOCA_N(VALUE, size - s->shape[0] - 1);

  if (NM_DTYPE(self) == nm::RUBYOBJ) {
    for (size_t i = 0; i < size - s->shape[0] - 1; ++i) {
      vals[i] = reinterpret_cast<VALUE*>(s->a)[s->shape[0] + 1 + i];
    }
  } else {
    for (size_t i = 0; i < size - s->shape[0] - 1; ++i) {
      vals[i] = rubyobj_from_cval((char*)(s->a) + DTYPE_SIZES[s->dtype]*(s->shape[0] + 1 + i), s->dtype).rval;
    }
  }

  VALUE ary = rb_ary_new4(size - s->shape[0] - 1, vals);

  for (size_t i = size; i < s->capacity; ++i)
    rb_ary_push(ary, Qnil);

  return ary;
}

/*
 * call-seq:
 *     yale_ia -> Array
 *
 * Get the IA portion of the IJA array of a Yale matrix. This gives the start and end positions of rows in the
 * JA and LU portions of the IJA and A arrays, respectively.
 */
static VALUE nm_ia(VALUE self) {
  YALE_STORAGE* s = reinterpret_cast<YALE_STORAGE*>(NM_SRC(self));

  VALUE* vals = ALLOCA_N(VALUE, s->shape[0] + 1);

  for (size_t i = 0; i < s->shape[0] + 1; ++i) {
    vals[i] = rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[s->itype]*i, s->itype).rval;
  }

  return rb_ary_new4(s->shape[0]+1, vals);
}

/*
 * call-seq:
 *     yale_ja -> Array
 *
 * Get the JA portion of the IJA array of a Yale matrix. This gives the column indices for entries in corresponding
 * positions in the LU portion of the A array.
 */
static VALUE nm_ja(VALUE self) {
  YALE_STORAGE* s = reinterpret_cast<YALE_STORAGE*>(NM_SRC(self));

  size_t size = nm_yale_storage_get_size(s);

  VALUE* vals = ALLOCA_N(VALUE, size - s->shape[0] - 1);

  for (size_t i = 0; i < size - s->shape[0] - 1; ++i) {
    vals[i] = rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[s->itype]*(s->shape[0] + 1 + i), s->itype).rval;
  }

  VALUE ary = rb_ary_new4(size - s->shape[0] - 1, vals);

  for (size_t i = size; i < s->capacity; ++i)
    rb_ary_push(ary, Qnil);

  return ary;
}

/*
 * call-seq:
 *     yale_ija -> Array
 *     yale_ija(index) -> ...
 *
 * Get the IJA array of a Yale matrix (or a component of the IJA array).
 */
static VALUE nm_ija(int argc, VALUE* argv, VALUE self) {
  VALUE idx;
  rb_scan_args(argc, argv, "01", &idx);

  YALE_STORAGE* s = reinterpret_cast<YALE_STORAGE*>(NM_SRC(self));
  size_t size = nm_yale_storage_get_size(s);

  if (idx == Qnil) {

    VALUE* vals = ALLOCA_N(VALUE, size);

    for (size_t i = 0; i < size; ++i) {
      vals[i] = rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[s->itype]*i, s->itype).rval;
    }

   VALUE ary = rb_ary_new4(size, vals);

    for (size_t i = size; i < s->capacity; ++i)
      rb_ary_push(ary, Qnil);

    return ary;

  } else {
    size_t index = FIX2INT(idx);
    if (index >= size) rb_raise(rb_eRangeError, "out of range");

    return rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[s->itype] * index, s->itype).rval;
  }
}


/*
 * call-seq:
 *     yale_nd_row -> ...
 *
 * This function gets the non-diagonal contents of a Yale matrix row.
 * The first argument should be the row index. The optional second argument may be :hash or :keys, but defaults
 * to :hash. If :keys is given, it will only return the Hash keys (the column indices).
 *
 * This function is meant to accomplish its purpose as efficiently as possible. It does not check for appropriate
 * range.
 */
static VALUE nm_nd_row(int argc, VALUE* argv, VALUE self) {
  if (NM_SRC(self) != NM_STORAGE(self))
    rb_raise(rb_eNotImpError, "must be called on a real matrix and not a slice");

  VALUE i_, as;
  rb_scan_args(argc, argv, "11", &i_, &as);

  bool keys = false;
  if (as != Qnil && rb_to_id(as) != nm_rb_hash) keys = true;

  size_t i = FIX2INT(i_);

  YALE_STORAGE* s   = NM_STORAGE_YALE(self);
  nm::dtype_t dtype = NM_DTYPE(self);
  nm::itype_t itype = NM_ITYPE(self);

  // get the position as a size_t
  // TODO: Come up with a faster way to get this than transforming to a Ruby object first.
  size_t pos = FIX2INT(rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[itype]*i, itype).rval);
  size_t nextpos = FIX2INT(rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[itype]*(i+1), itype).rval);
  size_t diff = nextpos - pos;

  VALUE ret;
  if (keys) {
    ret = rb_ary_new3(diff);

    for (size_t idx = pos; idx < nextpos; ++idx) {
      rb_ary_store(ret, idx - pos, rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[s->itype]*idx, s->itype).rval);
    }

  } else {
    ret = rb_hash_new();

    for (size_t idx = pos; idx < nextpos; ++idx) {
      rb_hash_aset(ret, rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[s->itype]*idx, s->itype).rval,
                        rubyobj_from_cval((char*)(s->a) + DTYPE_SIZES[s->dtype]*idx, s->dtype).rval);
    }
  }

  return ret;
}

/*
 * call-seq:
 *     yale_vector_set(i, column_index_array, cell_contents_array, pos) -> Fixnum
 *
 * Insert at position pos an array of non-diagonal elements with column indices given. Note that the column indices and values
 * must be storage-contiguous -- that is, you can't insert them around existing elements in some row, only amid some
 * elements in some row. You *can* insert them around a diagonal element, since this is stored separately. This function
 * may not be used for the insertion of diagonal elements in most cases, as these are already present in the data
 * structure and are typically modified by replacement rather than insertion.
 *
 * The last argument, pos, may be nil if you want to insert at the beginning of a row. Otherwise it needs to be provided.
 * Don't expect this function to know the difference. It really does very little checking, because its goal is to make
 * multiple contiguous insertion as quick as possible.
 *
 * You should also not attempt to insert values which are the default (0). These are not supposed to be stored, and may
 * lead to undefined behavior.
 *
 * Example:
 *    m.yale_vector_set(3, [0,3,4], [1,1,1], 15)
 *
 * The example above inserts the values 1, 1, and 1 in columns 0, 3, and 4, assumed to be located at position 15 (which
 * corresponds to row 3).
 *
 * Example:
 *    next = m.yale_vector_set(3, [0,3,4], [1,1,1])
 *
 * This example determines that i=3 is at position 15 automatically. The value returned, next, is the position where the
 * next value(s) should be inserted.
 */
VALUE nm_vector_set(int argc, VALUE* argv, VALUE self) { //, VALUE i_, VALUE jv, VALUE vv, VALUE pos_) {

  if (NM_SRC(self) != NM_STORAGE(self))
    rb_raise(rb_eNotImpError, "must be called on a real matrix and not a slice");

  // i, jv, vv are mandatory; pos is optional; thus "31"
  VALUE i_, jv, vv, pos_;
  rb_scan_args(argc, argv, "31", &i_, &jv, &vv, &pos_);

  size_t len   = RARRAY_LEN(jv); // need length in order to read the arrays in
  size_t vvlen = RARRAY_LEN(vv);

  if (len != vvlen)
    rb_raise(rb_eArgError, "lengths must match between j array (%d) and value array (%d)", len, vvlen);

  YALE_STORAGE* s   = NM_STORAGE_YALE(self);
  nm::dtype_t dtype = NM_DTYPE(self);
  nm::itype_t itype = NM_ITYPE(self);

  size_t i   = FIX2INT(i_);    // get the row

  // get the position as a size_t
  // TODO: Come up with a faster way to get this than transforming to a Ruby object first.
  if (pos_ == Qnil) pos_ = rubyobj_from_cval_by_itype((char*)(s->ija) + ITYPE_SIZES[itype]*i, itype).rval;
  size_t pos = FIX2INT(pos_);

  // Allocate the j array and the values array
  size_t* j  = ALLOCA_N(size_t, len);
  void* vals = ALLOCA_N(char, DTYPE_SIZES[dtype] * len);

  // Copy array contents
  for (size_t idx = 0; idx < len; ++idx) {
    j[idx] = FIX2INT(rb_ary_entry(jv, idx));
    rubyval_to_cval(rb_ary_entry(vv, idx), dtype, (char*)vals + idx * DTYPE_SIZES[dtype]);
  }

  nm_yale_storage_vector_insert(s, pos, j, vals, len, false, dtype, itype);
  nm_yale_storage_increment_ia_after(s, s->shape[0], i, len, itype);
  s->ndnz += len;

  // Return the updated position
  pos += len;
  return INT2FIX(pos);
}




/*
 * call-seq:
 *     __yale_default_value__ -> ...
 *
 * Get the default_value property from a yale matrix.
 */
VALUE nm_yale_default_value(VALUE self) {
  return default_value(NM_STORAGE_YALE(self));
}


/*
 * call-seq:
 *     __yale_map_merged_stored__(right) -> Enumerator
 *
 * A map operation on two Yale matrices which only iterates across the stored indices.
 */
VALUE nm_yale_map_merged_stored(VALUE left, VALUE right, VALUE init) {
  YALE_STORAGE *s = NM_STORAGE_YALE(left),
               *t = NM_STORAGE_YALE(right);

  ITYPE_TEMPLATE_TABLE(nm::yale_storage::map_merged_stored, VALUE, VALUE l, VALUE r, VALUE init, nm::itype_t)

  nm::itype_t itype = NM_MAX_ITYPE(s->itype, t->itype);
  return ttable[itype](left, right, init, itype);
}


/*
 * call-seq:
 *     __yale_map_stored__ -> Enumerator
 *
 * A map operation on two Yale matrices which only iterates across the stored indices.
 */
VALUE nm_yale_map_stored(VALUE self) {
  ITYPE_TEMPLATE_TABLE(nm::yale_storage::map_stored, VALUE, VALUE)

  return ttable[NM_ITYPE(self)](self);
}

} // end of extern "C" block

# tsvec.q -- generic growable vector, header-only (via domqlib Q).
#
# Parameters:
#   T      element type, e.g. "TS_Value" or "char *"
#   PREFIX naming prefix, e.g. ts_valvec
#   FREE   element destructor, called as FREE(&slot) on removal, e.g.
#          ts_value_free or ts_std_free_cstr
#
# The vector hands out zeroed slots (ts_XXX_add returns a pointer the
# caller fills in), so no COPY hook is needed.

@module tsvec
@version 1.0

@param T
@param PREFIX tsvec
@param FREE free_elem

@include <stddef.h>
@include <stdlib.h>
@include <string.h>

@guard

@struct {
    typedef struct {
        $T *data;
        size_t len;
        size_t cap;
    } ${PREFIX}_t;
}

@fn init {
    static inline void
    ${PREFIX}_init(${PREFIX}_t *v) {
        v->data = NULL;
        v->len = 0;
        v->cap = 0;
    }
}

@fn free {
    static inline void
    ${PREFIX}_free(${PREFIX}_t *v) {
        size_t i;
        if (!v)
            return;
        for (i = 0; i < v->len; i++)
            ${FREE}(&v->data[i]);
        free(v->data);
        v->data = NULL;
        v->len = 0;
        v->cap = 0;
    }
}

@fn reserve {
    static inline int
    ${PREFIX}_reserve(${PREFIX}_t *v, size_t want) {
        if (want > v->cap) {
            size_t nc = v->cap ? v->cap * 2 : 8;
            void *nv;
            while (nc < want)
                nc *= 2;
            nv = realloc(v->data, nc * sizeof(v->data[0]));
            if (!nv)
                return -1;
            v->data = nv;
            v->cap = nc;
        }
        return 0;
    }
}

@fn add {
    static inline $T *
    ${PREFIX}_add(${PREFIX}_t *v) {
        $T *slot;
        if (${PREFIX}_reserve(v, v->len + 1) != 0)
            return NULL;
        slot = &v->data[v->len++];
        memset(slot, 0, sizeof(*slot));
        return slot;
    }
}

@fn len {
    static inline size_t
    ${PREFIX}_len(const ${PREFIX}_t *v) {
        return v ? v->len : 0;
    }
}

@fn get {
    static inline $T *
    ${PREFIX}_get(${PREFIX}_t *v, size_t i) {
        if (!v || i >= v->len)
            return NULL;
        return &v->data[i];
    }
}

@fn pop {
    static inline void
    ${PREFIX}_pop(${PREFIX}_t *v) {
        if (!v || v->len == 0)
            return;
        v->len--;
        ${FREE}(&v->data[v->len]);
        memset(&v->data[v->len], 0, sizeof(v->data[0]));
    }
}

@fn clear {
    static inline void
    ${PREFIX}_clear(${PREFIX}_t *v) {
        size_t i;
        if (!v)
            return;
        for (i = 0; i < v->len; i++) {
            ${FREE}(&v->data[i]);
            memset(&v->data[i], 0, sizeof(v->data[0]));
        }
        v->len = 0;
    }
}

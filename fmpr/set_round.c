/*=============================================================================

    This file is part of ARB.

    ARB is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    ARB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ARB; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

=============================================================================*/
/******************************************************************************

    Copyright (C) 2012 Fredrik Johansson

******************************************************************************/

#include "fmpr.h"

static __inline__ void
_fmpz_add_ui(fmpz_t z, const fmpz_t x, ulong y)
{
    fmpz f = *x;

    if (!COEFF_IS_MPZ(f) && y <= COEFF_MAX)
        fmpz_set_si(z, f + y);
    else
        fmpz_add_ui(z, x, y);
}

static __inline__ int
rounds_up(fmpr_rnd_t rnd, int negative)
{
    if (rnd == FMPR_RND_DOWN) return 0;
    if (rnd == FMPR_RND_UP) return 1;
    if (rnd == FMPR_RND_FLOOR) return negative;
    return !negative;
}

/* like mpn_scan0b, but takes an upper size */
static __inline__ mp_bitcnt_t
mpn_scan0b(mp_srcptr up, mp_size_t size, mp_bitcnt_t from_bit)
{
    mp_limb_t t;
    long i, c;

    i = from_bit / GMP_NUMB_BITS;
    c = from_bit % FLINT_BITS;
    t = ((~up[i]) >> c) << c;

    while (t == 0)
    {
        i++;
        if (i == size)
            return size * FLINT_BITS;
        else
            t = ~up[i];
    }

    count_trailing_zeros(c, t);
    return (i * FLINT_BITS) + c;
}

static __inline__ void
tdiv_1(fmpz_t f, const fmpz_t g, ulong exp)
{
    __mpz_struct * z = _fmpz_promote(f);
    __mpz_struct * h = COEFF_TO_PTR(*g);
    mpz_tdiv_q_2exp(z, h, exp);
}

static __inline__ void
tdiv_2(fmpz_t f, __mpz_struct * g, ulong exp)
{
    __mpz_struct * z = _fmpz_promote(f);
    mpz_tdiv_q_2exp(z, g, exp);
}


long
_fmpr_set_round(fmpz_t rman, fmpz_t rexp,
    const fmpz_t man, const fmpz_t exp, long prec, fmpr_rnd_t rnd)
{
    if (!COEFF_IS_MPZ(*man))
    {
        long lead, trail, bc, v, w, shift, ret;
        int negative;

        v = *man;

        count_trailing_zeros(trail, v);
        v >>= trail;
        shift = trail;
        ret = FMPR_PREC_EXACT;

        /* may need to round */
        if (prec < FLINT_BITS - 2 - trail)
        {
            if (v < 0)
            {
                negative = 1;
                w = -v;
            }
            else
            {
                negative = 0;
                w = v;
            }

            count_leading_zeros(lead, w);
            bc = FLINT_BITS - lead;

            /* round */
            if (prec < bc)
            {
                w = (w >> (bc - prec)) + rounds_up(rnd, negative);
                shift += bc - prec;
                count_trailing_zeros(trail, w);
                w >>= trail;
                shift += trail;
                v = negative ? -w : w;
                ret = trail;
            }
        }

        /* the input is small, so the output must be small too */
        _fmpz_demote(rman);
        *rman = v;
        _fmpz_add_ui(rexp, exp, shift);
        return ret;
    }
    else
    {
        long size, bc, val, val_bits, val_limbs, ret, old_val;
        int negative, increment;
        mp_ptr d;
        __mpz_struct * z = COEFF_TO_PTR(*man);

        size = z->_mp_size;
        negative = size < 0;
        size = FLINT_ABS(size);
        d = z->_mp_d;

        /* bit size */
        count_leading_zeros(bc, d[size - 1]);
        bc = FLINT_BITS - bc;
        bc += (size - 1) * FLINT_BITS;

        /* quick exit */
        if (bc <= prec && (d[0] & 1))
        {
            if (rman != man) fmpz_set(rman, man);
            if (rexp != exp) fmpz_set(rexp, exp);
            return FMPR_PREC_EXACT;
        }

        /* trailing zeros */
        val_limbs = 0;
        while (d[val_limbs] == 0)
            val_limbs++;
        count_trailing_zeros(val_bits, d[val_limbs]);
        val = val_bits + (val_limbs * FLINT_BITS);

        /* no rounding necessary; just copy or shift to destination */
        if (bc - val <= prec)
        {
            ret = FMPR_PREC_EXACT;
            increment = 0;
        }
        else
        {
            /* truncation */
            if (!rounds_up(rnd, negative))
            {
                old_val = val;
                val = mpn_scan1(d, bc - prec);
                increment = 0;
            }
            /* round to next higher odd mantissa */
            else
            {
                old_val = val;
                val = mpn_scan0b(d, size, bc - prec);

                /* can overflow to next power of 2 */
                if (val == bc)
                {
                    fmpz_set_si(rman, negative ? -1 : 1);
                    _fmpz_add_ui(rexp, exp, bc);
                    return prec;
                }

                /* otherwise, incrementing will not cause overflow below */
                increment = 1;
            }

            val_limbs = val / FLINT_BITS;
            val_bits = val % FLINT_BITS;
            ret = prec - (bc - val);
        }

        /* the output mantissa is a small fmpz */
        if (bc - val <= FLINT_BITS - 2)
        {
            mp_limb_t h;
            if (val_limbs + 1 == size || val_bits == 0)
                h = d[val_limbs] >> val_bits;
            else
                h = (d[val_limbs] >> val_bits)
                    | (d[val_limbs + 1] << (FLINT_BITS - val_bits));
            h += increment;
            _fmpz_demote(rman);
            *rman = negative ? -h : h;
        }
        /* the output mantissa is an mpz */
        else
        {
            if (rman == man)
            {
                mpz_tdiv_q_2exp(z, z, val);
                if (increment) z->_mp_d[0]++;
            }
            else
            {
#if 1
                tdiv_1(rman, man, val);
#else
                /* broken */
                tdiv_2(rman, COEFF_TO_PTR(*man), val);
#endif

                if (increment)
                    COEFF_TO_PTR(*rman)->_mp_d[0]++;
            }
        }

        _fmpz_add_ui(rexp, exp, val);
        return ret;
    }
}

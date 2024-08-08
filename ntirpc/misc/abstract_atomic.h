/*
 * Copyright (c) 2012 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Permission to relicense his contributions under the above license terms
 * was given by Frank S. Filz, in #ganesha (freenode), on 08/08/2012.
 */

/**
 * @file   abstract_atomic.h
 * @author Adam C. Emerson <aemerson@linuxbox.com>
 * @author Frank S. Filz <ffilz@us.ibm.com>
 * @brief  Shim for compiler or library supplied atomic operations
 *
 * This file provides inline functions that provide atomic operations
 * appropriate to the compiler being used.  (Someone can add support
 * for an appropriate library later on.)
 *
 * The types functions are provided for:
 *
 * ptrdiff_t (fetch and store only)
 * time_t (fetch and store only)
 * void* (fetch and store only)
 * uintptr_t (fetch and store only)
 * int64_t
 * uint64_t
 * int32_t
 * uint21_t
 * int16_t
 * uint16_t
 * int8_t
 * uint8_t
 * size_t
 *
 * The functions provided are (using int64_t for example):
 *
 * int64_t atomic_add_int64_t(int64_t *augend, int64_t addend)
 * int64_t atomic_inc_int64_t(int64_t *var)
 * int64_t atomic_sub_int64_t(int64_t *minuend, int64_t subtrahend)
 * int64_t atomic_dec_int64_t(int64_t *var)
 * int64_t atomic_postadd_int64_t(int64_t *augend, int64_t addend)
 * int64_t atomic_postinc_int64_t(int64_t *var)
 * int64_t atomic_postsub_int64_t(int64_t *minuend, int64_t subtrahend)
 * int64_t atomic_postdec_int64_t(int64_t *var)
 * int64_t atomic_fetch_int64_t(int64_t *var)
 * void atomic_store_int64_t(int64_t *var, int64_t val)
 *
 * The following bit mask operations are provided for
 * uint64_t, uint32_t, uint_16t, and uint8_t:
 *
 * uint64_t atomic_clear_uint64_t_bits(uint64_t *var, uint64_t bits)
 * uint64_t atomic_set_uint64_t_bits(uint64_t *var, uint64_t bits)
 * uint64_t atomic_postclear_uint64_t_bits(uint64_t *var,
 * uint64_t atomic_postset_uint64_t_bits(uint64_t *var,
 *
 */

#ifndef _ABSTRACT_ATOMIC_H
#define _ABSTRACT_ATOMIC_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * Preaddition, presubtraction, preincrement, predecrement (return the
 * value after the operation, by analogy with the ++n preincrement
 * operator.)
 */

/**
 * @brief Atomically add to an int64_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline int64_t atomic_add_int64_t(int64_t *augend, int64_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment an int64_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline int64_t atomic_inc_int64_t(int64_t *var)
{
	return atomic_add_int64_t(var, 1);
}

/**
 * @brief Atomically subtract from an int64_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value after subtraction.
 */

static inline int64_t atomic_sub_int64_t(int64_t *minuend, int64_t subtrahend)
{
	return __atomic_sub_fetch(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement an int64_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 */

static inline int64_t atomic_dec_int64_t(int64_t *var)
{
	return atomic_sub_int64_t(var, 1);
}

/**
 * @brief Atomically add to an int64_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline uint64_t atomic_add_uint64_t(uint64_t *augend, uint64_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a uint64_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline uint64_t atomic_inc_uint64_t(uint64_t *var)
{
	return atomic_add_uint64_t(var, 1);
}

/**
 * @brief Atomically subtract from a uint64_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value after subtraction.
 */

static inline uint64_t atomic_sub_uint64_t(uint64_t *minuend,
					   uint64_t subtrahend)
{
	return __atomic_sub_fetch(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a uint64_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after decrement.
 */

static inline uint64_t atomic_dec_uint64_t(uint64_t *var)
{
	return atomic_sub_uint64_t(var, 1);
}

/**
 * @brief Atomically add to an int32_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline int32_t atomic_add_int32_t(int32_t *augend, int32_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment an int32_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline int32_t atomic_inc_int32_t(int32_t *var)
{
	return atomic_add_int32_t(var, 1);
}

/**
 * @brief Atomically subtract from an int32_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value after subtraction.
 */

static inline int32_t atomic_sub_int32_t(int32_t *minuend, int32_t subtrahend)
{
	return __atomic_sub_fetch(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement an int32_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after decrement.
 */

static inline int32_t atomic_dec_int32_t(int32_t *var)
{
	return atomic_sub_int32_t(var, 1);
}

/**
 * @brief Atomically add to a uint32_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline uint32_t atomic_add_uint32_t(uint32_t *augend, uint32_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a uint32_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline uint32_t atomic_inc_uint32_t(uint32_t *var)
{
	return atomic_add_uint32_t(var, 1);
}

/**
 * @brief Atomically subtract from a uint32_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after subtraction.
 */

static inline uint32_t atomic_sub_uint32_t(uint32_t *var, uint32_t sub)
{
	return __atomic_sub_fetch(var, sub, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a uint32_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after decrement.
 */

static inline uint32_t atomic_dec_uint32_t(uint32_t *var)
{
	return atomic_sub_uint32_t(var, 1);
}

/**
 * @brief Atomically add to an int16_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline int16_t atomic_add_int16_t(int16_t *augend, int16_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment an int16_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline int16_t atomic_inc_int16_t(int16_t *var)
{
	return atomic_add_int16_t(var, 1);
}

/**
 * @brief Atomically subtract from an int16_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value after subtraction.
 */

static inline int16_t atomic_sub_int16_t(int16_t *minuend, int16_t subtrahend)
{
	return __atomic_sub_fetch(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement an int16_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after decrement.
 */

static inline int16_t atomic_dec_int16_t(int16_t *var)
{
	return atomic_sub_int16_t(var, 1);
}

/**
 * @brief Atomically add to a uint16_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline uint16_t atomic_add_uint16_t(uint16_t *augend, uint16_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a uint16_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline uint16_t atomic_inc_uint16_t(uint16_t *var)
{
	return atomic_add_uint16_t(var, 1);
}

/**
 * @brief Atomically subtract from a uint16_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value after subtraction.
 */

static inline uint16_t atomic_sub_uint16_t(uint16_t *minuend,
					   uint16_t subtrahend)
{
	return __atomic_sub_fetch(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a uint16_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after decrement.
 */

static inline uint16_t atomic_dec_uint16_t(uint16_t *var)
{
	return atomic_sub_uint16_t(var, 1);
}

/**
 * @brief Atomically add to an int8_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline int8_t atomic_add_int8_t(int8_t *augend, int8_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment an int8_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline int8_t atomic_inc_int8_t(int8_t *var)
{
	return atomic_add_int8_t(var, 1);
}

/**
 * @brief Atomically subtract from an int8_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value after subtraction.
 */

static inline int8_t atomic_sub_int8_t(int8_t *minuend, int8_t subtrahend)
{
	return __atomic_sub_fetch(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement an int8_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after decrement.
 */

static inline int8_t atomic_dec_int8_t(int8_t *var)
{
	return atomic_sub_int8_t(var, 1);
}

/**
 * @brief Atomically add to a uint8_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline uint8_t atomic_add_uint8_t(uint8_t *augend, int8_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a uint8_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline uint8_t atomic_inc_uint8_t(uint8_t *var)
{
	return atomic_add_uint8_t(var, 1);
}

/**
 * @brief Atomically subtract from a uint8_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value after subtraction.
 */

static inline uint8_t atomic_sub_uint8_t(uint8_t *minuend, uint8_t subtrahend)
{
	return __atomic_sub_fetch(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a uint8_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after decrement.
 */

static inline uint8_t atomic_dec_uint8_t(uint8_t *var)
{
	return atomic_sub_uint8_t(var, 1);
}

/**
 * @brief Atomically add to a size_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value after addition.
 */

static inline size_t atomic_add_size_t(size_t *augend, size_t addend)
{
	return __atomic_add_fetch(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a size_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after increment.
 */

static inline size_t atomic_inc_size_t(size_t *var)
{
	return atomic_add_size_t(var, 1);
}

/**
 * @brief Atomically subtract from a size_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value after subtraction.
 */

static inline size_t atomic_sub_size_t(size_t *minuend, size_t subtrahend)
{
	return __atomic_sub_fetch(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a size_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value after decrement.
 */

static inline size_t atomic_dec_size_t(size_t *var)
{
	return atomic_sub_size_t(var, 1);
}

/*
 * Postaddition, postsubtraction, postincrement, postdecrement (return the
 * value before the operation, by analogy with the n++ postincrement
 * operator.)
 */

/**
 * @brief Atomically add to an int64_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline int64_t atomic_postadd_int64_t(int64_t *augend, int64_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment an int64_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline int64_t atomic_postinc_int64_t(int64_t *var)
{
	return atomic_postadd_int64_t(var, 1);
}

/**
 * @brief Atomically subtract from an int64_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value before subtraction.
 */

static inline int64_t atomic_postsub_int64_t(int64_t *minuend,
					     int64_t subtrahend)
{
	return __atomic_fetch_sub(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement an int64_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 */

static inline int64_t atomic_postdec_int64_t(int64_t *var)
{
	return atomic_postsub_int64_t(var, 1);
}

/**
 * @brief Atomically add to an int64_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline uint64_t atomic_postadd_uint64_t(uint64_t *augend,
					       uint64_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a uint64_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline uint64_t atomic_postinc_uint64_t(uint64_t *var)
{
	return atomic_postadd_uint64_t(var, 1);
}

/**
 * @brief Atomically subtract from a uint64_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value before subtraction.
 */

static inline uint64_t atomic_postsub_uint64_t(uint64_t *minuend,
					       uint64_t subtrahend)
{
	return __atomic_fetch_sub(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a uint64_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before decrement.
 */

static inline uint64_t atomic_postdec_uint64_t(uint64_t *var)
{
	return atomic_postsub_uint64_t(var, 1);
}

/**
 * @brief Atomically add to an int32_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline int32_t atomic_postadd_int32_t(int32_t *augend, int32_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment an int32_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline int32_t atomic_postinc_int32_t(int32_t *var)
{
	return atomic_postadd_int32_t(var, 1);
}

/**
 * @brief Atomically subtract from an int32_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value before subtraction.
 */

static inline int32_t atomic_postsub_int32_t(int32_t *minuend,
					     int32_t subtrahend)
{
	return __atomic_fetch_sub(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement an int32_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before decrement.
 */

static inline int32_t atomic_postdec_int32_t(int32_t *var)
{
	return atomic_postsub_int32_t(var, 1);
}

/**
 * @brief Atomically add to a uint32_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline uint32_t atomic_postadd_uint32_t(uint32_t *augend,
					       uint32_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a uint32_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline uint32_t atomic_postinc_uint32_t(uint32_t *var)
{
	return atomic_postadd_uint32_t(var, 1);
}

/**
 * @brief Atomically subtract from a uint32_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before subtraction.
 */

static inline uint32_t atomic_postsub_uint32_t(uint32_t *var, uint32_t sub)
{
	return __atomic_fetch_sub(var, sub, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a uint32_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before decrement.
 */

static inline uint32_t atomic_postdec_uint32_t(uint32_t *var)
{
	return atomic_postsub_uint32_t(var, 1);
}

/**
 * @brief Atomically add to an int16_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline int16_t atomic_postadd_int16_t(int16_t *augend, int16_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment an int16_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline int16_t atomic_postinc_int16_t(int16_t *var)
{
	return atomic_postadd_int16_t(var, 1);
}

/**
 * @brief Atomically subtract from an int16_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value before subtraction.
 */

static inline int16_t atomic_postsub_int16_t(int16_t *minuend,
					     int16_t subtrahend)
{
	return __atomic_fetch_sub(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement an int16_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before decrement.
 */

static inline int16_t atomic_postdec_int16_t(int16_t *var)
{
	return atomic_postsub_int16_t(var, 1);
}

/**
 * @brief Atomically add to a uint16_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline uint16_t atomic_postadd_uint16_t(uint16_t *augend,
					       uint16_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a uint16_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline uint16_t atomic_postinc_uint16_t(uint16_t *var)
{
	return atomic_postadd_uint16_t(var, 1);
}

/**
 * @brief Atomically subtract from a uint16_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value before subtraction.
 */

static inline uint16_t atomic_postsub_uint16_t(uint16_t *minuend,
					       uint16_t subtrahend)
{
	return __atomic_fetch_sub(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a uint16_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before decrement.
 */

static inline uint16_t atomic_postdec_uint16_t(uint16_t *var)
{
	return atomic_postsub_uint16_t(var, 1);
}

/**
 * @brief Atomically add to an int8_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline int8_t atomic_postadd_int8_t(int8_t *augend, int8_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment an int8_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline int8_t atomic_postinc_int8_t(int8_t *var)
{
	return atomic_postadd_int8_t(var, 1);
}

/**
 * @brief Atomically subtract from an int8_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value before subtraction.
 */

static inline int8_t atomic_postsub_int8_t(int8_t *minuend, int8_t subtrahend)
{
	return __atomic_fetch_sub(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement an int8_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before decrement.
 */

static inline int8_t atomic_postdec_int8_t(int8_t *var)
{
	return atomic_postsub_int8_t(var, 1);
}

/**
 * @brief Atomically add to a uint8_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline uint8_t atomic_postadd_uint8_t(uint8_t *augend, uint8_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a uint8_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline uint8_t atomic_postinc_uint8_t(uint8_t *var)
{
	return atomic_postadd_uint8_t(var, 1);
}

/**
 * @brief Atomically subtract from a uint8_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value before subtraction.
 */

static inline uint8_t atomic_postsub_uint8_t(uint8_t *minuend,
					     uint8_t subtrahend)
{
	return __atomic_fetch_sub(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a uint8_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before decrement.
 */

static inline uint8_t atomic_postdec_uint8_t(uint8_t *var)
{
	return atomic_postsub_uint8_t(var, 1);
}

/**
 * @brief Atomically add to a size_t
 *
 * This function atomically adds to the supplied value.
 *
 * @param[in,out] augend Number to be added to
 * @param[in]     addend Number to add
 *
 * @return The value before addition.
 */

static inline size_t atomic_postadd_size_t(size_t *augend, size_t addend)
{
	return __atomic_fetch_add(augend, addend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically increment a size_t
 *
 * This function atomically adds 1 to the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before increment.
 */

static inline size_t atomic_postinc_size_t(size_t *var)
{
	return atomic_postadd_size_t(var, 1);
}

/**
 * @brief Atomically subtract from a size_t
 *
 * This function atomically subtracts from the supplied value.
 *
 * @param[in,out] minuend    Number to be subtracted from
 * @param[in]     subtrahend Number to subtract
 *
 * @return The value before subtraction.
 */

static inline size_t atomic_postsub_size_t(size_t *minuend, size_t subtrahend)
{
	return __atomic_fetch_sub(minuend, subtrahend, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically decrement a size_t
 *
 * This function atomically subtracts 1 from the supplied value.
 *
 * @param[in,out] var Pointer to the variable to modify
 *
 * @return The value before decrement.
 */

static inline size_t atomic_postdec_size_t(size_t *var)
{
	return atomic_postsub_size_t(var, 1);
}

/*
 * Preclear and preset bits (return the value after the operation, by
 * analogy with the ++n preincrement operator.)
 */

/**
 * @brief Atomically clear bits in a uint64_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to clear
 *
 * @return The value after clearing.
 */

static inline uint64_t atomic_clear_uint64_t_bits(uint64_t *var, uint64_t bits)
{
	return __atomic_and_fetch(var, ~bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically set bits in a uint64_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to set
 *
 * @return The value after setting.
 */

static inline uint64_t atomic_set_uint64_t_bits(uint64_t *var, uint64_t bits)
{
	return __atomic_or_fetch(var, bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically clear bits in a uint32_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to clear
 *
 * @return The value after clearing.
 */

static inline uint32_t atomic_clear_uint32_t_bits(uint32_t *var, uint32_t bits)
{
	return __atomic_and_fetch(var, ~bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically set bits in a uint32_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to set
 *
 * @return The value after setting.
 */

static inline uint32_t atomic_set_uint32_t_bits(uint32_t *var, uint32_t bits)
{
	return __atomic_or_fetch(var, bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically clear bits in a uint16_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to clear
 *
 * @return The value after clearing.
 */

static inline uint16_t atomic_clear_uint16_t_bits(uint16_t *var, uint16_t bits)
{
	return __atomic_and_fetch(var, ~bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically set bits in a uint16_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to set
 *
 * @return The value after setting.
 */

static inline uint16_t atomic_set_uint16_t_bits(uint16_t *var, uint16_t bits)
{
	return __atomic_or_fetch(var, bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically clear bits in a uint8_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to clear
 *
 * @return The value after clearing.
 */

static inline uint8_t atomic_clear_uint8_t_bits(uint8_t *var, uint8_t bits)
{
	return __atomic_and_fetch(var, ~bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically set bits in a uint8_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to set
 *
 * @return The value after setting.
 */

static inline uint8_t atomic_set_uint8_t_bits(uint8_t *var, uint8_t bits)
{
	return __atomic_or_fetch(var, bits, __ATOMIC_SEQ_CST);
}

/*
 * Postclear and postset bits (return the value before the operation,
 * by analogy with the n++ postincrement operator.)
 */

/**
 * @brief Atomically clear bits in a uint64_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to clear
 *
 * @return The value before clearing.
 */

static inline uint64_t atomic_postclear_uint64_t_bits(uint64_t *var,
						      uint64_t bits)
{
	return __atomic_fetch_and(var, ~bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically set bits in a uint64_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to set
 *
 * @return The value before setting.
 */

static inline uint64_t atomic_postset_uint64_t_bits(uint64_t *var,
						    uint64_t bits)
{
	return __atomic_fetch_or(var, bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically clear bits in a uint32_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to clear
 *
 * @return The value before clearing.
 */

static inline uint32_t atomic_postclear_uint32_t_bits(uint32_t *var,
						      uint32_t bits)
{
	return __atomic_fetch_and(var, ~bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically set bits in a uint32_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to set
 *
 * @return The value before setting.
 */

static inline uint32_t atomic_postset_uint32_t_bits(uint32_t *var,
						    uint32_t bits)
{
	return __atomic_fetch_or(var, bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically clear bits in a uint16_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to clear
 *
 * @return The value before clearing.
 */

static inline uint16_t atomic_postclear_uint16_t_bits(uint16_t *var,
						      uint16_t bits)
{
	return __atomic_fetch_and(var, ~bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically set bits in a uint16_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to set
 *
 * @return The value before setting.
 */

static inline uint16_t atomic_postset_uint16_t_bits(uint16_t *var,
						    uint16_t bits)
{
	return __atomic_fetch_or(var, bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically clear bits in a uint8_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to clear
 *
 * @return The value before clearing.
 */

static inline uint8_t atomic_postclear_uint8_t_bits(uint8_t *var, uint8_t bits)
{
	return __atomic_fetch_and(var, ~bits, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically set bits in a uint8_t
 *
 * This function atomic clears the bits indicated.
 *
 * @param[in,out] var  Pointer to the value to modify
 * @param[in]     bits Bits to set
 *
 * @return The value before setting.
 */

static inline uint8_t atomic_postset_uint8_t_bits(uint8_t *var, uint8_t bits)
{
	return __atomic_fetch_or(var, bits, __ATOMIC_SEQ_CST);
}

/*
 * Fetch and store
 */

/**
 * @brief Atomically fetch a size_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline size_t atomic_fetch_size_t(size_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a size_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_size_t(size_t *var, size_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a ptrdiff_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline ptrdiff_t atomic_fetch_ptrdiff_t(ptrdiff_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a ptrdiff_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_ptrdiff_t(ptrdiff_t *var, ptrdiff_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a time_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline time_t atomic_fetch_time_t(time_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a time_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_time_t(time_t *var, time_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a uintptr_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline uintptr_t atomic_fetch_uintptr_t(uintptr_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a uintptr_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_uintptr_t(uintptr_t *var, uintptr_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a void *
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline void *atomic_fetch_voidptr(void **var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a void *
 *
 * This function atomically stores the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_voidptr(void **var, void *val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch an int64_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline int64_t atomic_fetch_int64_t(int64_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store an int64_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_int64_t(int64_t *var, int64_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a uint64_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline uint64_t atomic_fetch_uint64_t(uint64_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a uint64_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_uint64_t(uint64_t *var, uint64_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch an int32_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline int32_t atomic_fetch_int32_t(int32_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store an int32_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_int32_t(int32_t *var, int32_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a uint32_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline uint32_t atomic_fetch_uint32_t(uint32_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a uint32_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_uint32_t(uint32_t *var, uint32_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch an int16_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline int16_t atomic_fetch_int16_t(int16_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store an int16_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_int16_t(int16_t *var, int16_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a uint16_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline uint16_t atomic_fetch_uint16_t(uint16_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a uint16_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_uint16_t(uint16_t *var, uint16_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a int8_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline int8_t atomic_fetch_int8_t(int8_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a int8_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_int8_t(int8_t *var, int8_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically fetch a uint8_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to fetch
 *
 * @return the value pointed to by var.
 */

static inline uint8_t atomic_fetch_uint8_t(uint8_t *var)
{
	return __atomic_load_n(var, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically store a uint8_t
 *
 * This function atomically fetches the value indicated by the
 * supplied pointer.
 *
 * @param[in,out] var Pointer to the variable to modify
 * @param[in]     val The value to store
 */

static inline void atomic_store_uint8_t(uint8_t *var, uint8_t val)
{
	__atomic_store_n(var, val, __ATOMIC_SEQ_CST);
}
#endif				/* !_ABSTRACT_ATOMIC_H */

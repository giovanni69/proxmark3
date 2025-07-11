//-----------------------------------------------------------------------------
// Borrowed initially from https://github.com/holiman/loclass
// Copyright (C) 2014 Martin Holst Swende
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// WARNING
//
// THIS CODE IS CREATED FOR EXPERIMENTATION AND EDUCATIONAL USE ONLY.
//
// USAGE OF THIS CODE IN OTHER WAYS MAY INFRINGE UPON THE INTELLECTUAL
// PROPERTY OF OTHER PARTIES, SUCH AS INSIDE SECURE AND HID GLOBAL,
// AND MAY EXPOSE YOU TO AN INFRINGEMENT ACTION FROM THOSE PARTIES.
//
// THIS CODE SHOULD NEVER BE USED TO INFRINGE PATENTS OR INTELLECTUAL PROPERTY RIGHTS.
//-----------------------------------------------------------------------------
// It is a reconstruction of the cipher engine used in iClass, and RFID techology.
//
// The implementation is based on the work performed by
// Flavio D. Garcia, Gerhard de Koning Gans, Roel Verdult and
// Milosch Meriac in the paper "Dismantling IClass".
//-----------------------------------------------------------------------------
/**


From "Dismantling iclass":
    This section describes in detail the built-in key diversification algorithm of iClass.
    Besides the obvious purpose of deriving a card key from a master key, this
    algorithm intends to circumvent weaknesses in the cipher by preventing the
    usage of certain ‘weak’ keys. In order to compute a diversified key, the iClass
    reader first encrypts the card identity id with the master key K, using single
    DES. The resulting ciphertext is then input to a function called hash0 which
    outputs the diversified key k.

    k = hash0(DES enc (id, K))

    Here the DES encryption of id with master key K outputs a cryptogram c
    of 64 bits. These 64 bits are divided as c = x, y, z [0] , . . . , z [7] ∈ F 82 × F 82 × (F 62 ) 8
    which is used as input to the hash0 function. This function introduces some
    obfuscation by performing a number of permutations, complement and modulo
    operations, see Figure 2.5. Besides that, it checks for and removes patterns like
    similar key bytes, which could produce a strong bias in the cipher. Finally, the
    output of hash0 is the diversified card key k = k [0] , . . . , k [7] ∈ (F 82 ) 8 .

**/
#include "ikeys.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "commonutil.h"  // ARRAYLEN

#include "fileutils.h"
#include "cipherutils.h"
#include "mbedtls/des.h"

uint8_t pi[35] = {
    0x0F, 0x17, 0x1B, 0x1D, 0x1E, 0x27, 0x2B, 0x2D,
    0x2E, 0x33, 0x35, 0x39, 0x36, 0x3A, 0x3C, 0x47,
    0x4B, 0x4D, 0x4E, 0x53, 0x55, 0x56, 0x59, 0x5A,
    0x5C, 0x63, 0x65, 0x66, 0x69, 0x6A, 0x6C, 0x71,
    0x72, 0x74, 0x78
};

/**
 * @brief The key diversification algorithm uses 6-bit bytes.
 * This implementation uses 64 bit uint to pack seven of them into one
 * variable. When they are there, they are placed as follows:
 * XXXX XXXX N0 .... N7, occupying the last 48 bits.
 *
 * This function picks out one from such a collection
 * @param all
 * @param n bitnumber
 * @return
 */
//#define getSixBitByte(c, n)  ((uint8_t)(((c) >> (42 - 6 * (n))) & 0x3F))

static inline uint8_t getSixBitByte(uint64_t c, int n) {
    return (c >> (42 - 6 * n)) & 0x3F;
}

/**
 * @brief Puts back a six-bit 'byte' into a uint64_t.
 * @param c buffer
 * @param z the value to place there
 * @param n bitnumber.
 */

static void pushbackSixBitByte(uint64_t *c, uint8_t z, int n) {
    //0x XXXX YYYY ZZZZ ZZZZ ZZZZ
    //             ^z0         ^z7
    //z0:  1111 1100 0000 0000

    uint64_t masked = z & 0x3F;
    uint64_t eraser = 0x3F;
    masked <<= 42 - 6 * n;
    eraser <<= 42 - 6 * n;

    //masked <<= 6*n;
    //eraser <<= 6*n;

    eraser = ~eraser;
    (*c) &= eraser;
    (*c) |= masked;

}
/**
 * @brief Swaps the z-values.
 * If the input value has format XYZ0Z1...Z7, the output will have the format
 * XYZ7Z6...Z0 instead
 * @param c
 * @return
 */
static uint64_t swapZvalues(uint64_t c) {
    uint64_t newz = 0;
    pushbackSixBitByte(&newz, getSixBitByte(c, 0), 7);
    pushbackSixBitByte(&newz, getSixBitByte(c, 1), 6);
    pushbackSixBitByte(&newz, getSixBitByte(c, 2), 5);
    pushbackSixBitByte(&newz, getSixBitByte(c, 3), 4);
    pushbackSixBitByte(&newz, getSixBitByte(c, 4), 3);
    pushbackSixBitByte(&newz, getSixBitByte(c, 5), 2);
    pushbackSixBitByte(&newz, getSixBitByte(c, 6), 1);
    pushbackSixBitByte(&newz, getSixBitByte(c, 7), 0);
    newz |= (c & 0xFFFF000000000000);
    return newz;
}

/**
* @return 4 six-bit bytes chunked into a uint64_t,as 00..00a0a1a2a3
*/
static uint64_t ck(int i, int j, uint64_t z) {
    if (i == 1 && j == -1) {
        // ck(1, −1, z [0] . . . z [3] ) = z [0] . . . z [3]
        return z;
    } else if (j == -1) {
        // ck(i, −1, z [0] . . . z [3] ) = ck(i − 1, i − 2, z [0] . . . z [3] )
        return ck(i - 1, i - 2, z);
    }

    if (getSixBitByte(z, i) == getSixBitByte(z, j)) {
        //ck(i, j − 1, z [0] . . . z [i] ← j . . . z [3] )
        uint64_t newz = 0;
        for (int c = 0; c < 4; c++) {
            uint8_t val = getSixBitByte(z, c);
            if (c == i)
                pushbackSixBitByte(&newz, j, c);
            else
                pushbackSixBitByte(&newz, val, c);
        }
        return ck(i, j - 1, newz);
    } else {
        return ck(i, j - 1, z);
    }
}
/**

    Definition 8.
    Let the function check : (F 62 ) 8 → (F 62 ) 8 be defined as
    check(z [0] . . . z [7] ) = ck(3, 2, z [0] . . . z [3] ) · ck(3, 2, z [4] . . . z [7] )

    where ck : N × N × (F 62 ) 4 → (F 62 ) 4 is defined as

        ck(1, −1, z [0] . . . z [3] ) = z [0] . . . z [3]
        ck(i, −1, z [0] . . . z [3] ) = ck(i − 1, i − 2, z [0] . . . z [3] )
        ck(i, j, z [0] . . . z [3] ) =
        ck(i, j − 1, z [0] . . . z [i] ← j . . . z [3] ),  if z [i] = z [j] ;
        ck(i, j − 1, z [0] . . . z [3] ), otherwise

    otherwise.
**/

static uint64_t check(uint64_t z) {
    //These 64 bits are divided as c = x, y, z [0] , . . . , z [7]

    // ck(3, 2, z [0] . . . z [3] )
    uint64_t ck1 = ck(3, 2, z);

    // ck(3, 2, z [4] . . . z [7] )
    uint64_t ck2 = ck(3, 2, z << 24);

    //The ck function will place the values
    // in the middle of z.
    ck1 &= 0x00000000FFFFFF000000;
    ck2 &= 0x00000000FFFFFF000000;

    return ck1 | ck2 >> 24;
}

// Reverse ck (scramble-1)
static uint64_t reverse_ck(int i, int j, uint64_t z) {
    if (i == 1 && j == -1) {
        return z;
    } else if (j == -1) {
        return reverse_ck(i - 1, i - 2, z);
    }

    uint64_t newz = 0;
    if (getSixBitByte(z, i) == j) { // Reverse the swap logic based on condition in scramble^{-1}
        // Perform reverse swap
        for (int c = 0; c < 4; c++) {
            uint8_t val = getSixBitByte(z, c);
            if (c == i) {
                pushbackSixBitByte(&newz, getSixBitByte(z, j), c);
            } else {
                pushbackSixBitByte(&newz, val, c);
            }
        }
        return reverse_ck(i, j - 1, newz);
    } else {
        // Continue recursion
        return reverse_ck(i, j - 1, z);
    }
}

static uint64_t reverse_check(uint64_t z) {

    //retrieve ck1 and ck2 from the hash

    // Step 1: Extract ck2 shifted part from result
    // Assuming ck2 shifted part is from bits 0-23 of the result
    uint64_t shifted_ck2_part = z & 0x0000000000FFFFFF; // Mask the lower 24 bits

    // Step 2: Reconstruct ck2
    uint64_t ck2 = shifted_ck2_part << 24; // Shift back to get original ck2 value
    // Step 3: Recover ck1
    uint64_t ck1 = z & ~(ck2 >> 24); // Clear the bits where ck2 affected the result
    // Now ck1 and ck2 have their original values before (after ck function took place)

    ck1 = reverse_ck(3, 2, ck1);
    ck2 = reverse_ck(3, 2, ck2);

    return ck1 | ck2 >> 24; //This is now zP

}

static void printState(const char *desc, uint64_t c) {
    if (g_debugMode == 0)
        return;

    char s[60] = {0};
    snprintf(s, sizeof(s), "%s : ", desc);

    uint8_t x = (c & 0xFF00000000000000) >> 56;
    uint8_t y = (c & 0x00FF000000000000) >> 48;

    snprintf(s + strlen(s), sizeof(s) - strlen(s), "  %02x %02x", x, y);

    for (uint8_t i = 0; i < 8; i++)
        snprintf(s + strlen(s), sizeof(s) - strlen(s), " %02x", getSixBitByte(c, i));

    PrintAndLogEx(DEBUG, "%s", s);
}

static void permute(BitstreamIn_t *p_in, uint64_t z, int l, int r, BitstreamOut_t *out) {
    if (bitsLeft(p_in) == 0)
        return;
    bool pn = tailBit(p_in);
    if (pn) {
        // pn = 1
        uint8_t zl = getSixBitByte(z, l);
        push6bits(out, zl + 1);
        permute(p_in, z, l + 1, r, out);
    } else {
        // otherwise
        uint8_t zr = getSixBitByte(z, r);
        push6bits(out, zr);
        permute(p_in, z, l, r + 1, out);
    }
}

static void reverse_permute(BitstreamIn_t *p_in, uint64_t z, int l, BitstreamOut_t *out1, BitstreamOut_t *out2, bool fix) {
    if (bitsLeft(p_in) == 0)
        return;
    bool pn = tailBit(p_in);
    if (pn) { //if p == 1 for that six bit position, then sum it
        // pn = 1
        uint8_t zl = getSixBitByte(z, l);
        if (fix) {
            push6bits(out1, zl - 1);
        } else {
            push6bits(out1, zl);
        }
    } else {
        // otherwise
        uint8_t zr = getSixBitByte(z, l);
        push6bits(out2, zr);
    }
    reverse_permute(p_in, z, l + 1, out1, out2, fix);
}

/**
 * @brief
 *Definition 11. Let the function hash0 : F 82 × F 82 × (F 62 ) 8 → (F 82 ) 8 be defined as
 *  hash0(x, y, z [0] . . . z [7] ) = k [0] . . . k [7] where
 * z'[i] = (z[i] mod (63-i)) + i      i =  0...3
 * z'[i+4] = (z[i+4] mod (64-i)) + i  i =  0...3
 * ẑ = check(z');
 * @param c
 * @param k this is where the diversified key is put (should be 8 bytes)
 * @return
 */
void hash0(uint64_t c, uint8_t k[8]) {
    c = swapZvalues(c);

    if (g_debugMode > 0) {
        PrintAndLogEx(DEBUG, "          | x| y|z0|z1|z2|z3|z4|z5|z6|z7|");
        printState("origin", c);
    }
    //These 64 bits are divided as c = x, y, z [0] , . . . , z [7]
    // x = 8 bits
    // y = 8 bits
    // z0-z7 6 bits each : 48 bits
    uint8_t x = (c & 0xFF00000000000000) >> 56;
    uint8_t y = (c & 0x00FF000000000000) >> 48;
    uint64_t zP = 0;

    for (int n = 0;  n < 4 ; n++) {
        uint8_t zn = getSixBitByte(c, n);
        uint8_t zn4 = getSixBitByte(c, n + 4);
        uint8_t _zn = (zn % (63 - n)) + n;
        uint8_t _zn4 = (zn4 % (64 - n)) + n;

        pushbackSixBitByte(&zP, _zn, n);
        pushbackSixBitByte(&zP, _zn4, n + 4);
    }

    if (g_debugMode > 0) printState("0|0|z'", zP);

    uint64_t zCaret = check(zP);

    if (g_debugMode > 0) printState("0|0|z^", zP);

    uint8_t p = pi[x % 35];

    if (x & 1) //Check if x7 is 1
        p = ~p;

    if (g_debugMode > 0) PrintAndLogEx(DEBUG, "     p : %02x", p);

    BitstreamIn_t p_in = { &p, 8, 0 };
    uint8_t outbuffer[] = {0, 0, 0, 0, 0, 0, 0, 0};
    BitstreamOut_t out = {outbuffer, 0, 0};
    permute(&p_in, zCaret, 0, 4, &out); //returns 48 bits? or 6 8-bytes

    //Out is now a buffer containing six-bit bytes, should be 48 bits
    // if all went well
    //Shift z-values down onto the lower segment

    uint64_t zTilde = x_bytes_to_num(outbuffer, sizeof(outbuffer));

    zTilde >>= 16;

    if (g_debugMode > 0) printState("0|0|z~", zTilde);

    for (int i = 0; i < 8; i++) {
        // the key on index i is first a bit from y
        // then six bits from z,
        // then a bit from p

        // Init with zeroes
        k[i] = 0;
        // First, place yi leftmost in k
        //k[i] |= (y  << i) & 0x80 ;

        // First, place y(7-i) leftmost in k
        k[i] |= (y  << (7 - i)) & 0x80 ;

        uint8_t zTilde_i = getSixBitByte(zTilde, i);
        // zTildeI is now on the form 00XXXXXX
        // with one leftshift, it'll be
        // 0XXXXXX0
        // So after leftshift, we can OR it into k
        // However, when doing complement, we need to
        // again MASK 0XXXXXX0 (0x7E)
        zTilde_i <<= 1;

        //Finally, add bit from p or p-mod
        //Shift bit i into rightmost location (mask only after complement)
        uint8_t p_i = p >> i & 0x1;

        if (k[i]) { // yi = 1
            // PrintAndLogEx(NORMAL, "k[%d] + 1", i);
            k[i] |= ~zTilde_i & 0x7E;
            k[i] |= p_i & 1;
            k[i] += 1;

        } else { // otherwise
            k[i] |= zTilde_i & 0x7E;
            k[i] |= (~p_i) & 1;
        }
    }
}

static int find_p_in_pi(uint8_t p) {
    for (int i = 0; i < 35; i++) {
        if (pi[i] == p) {
            return i;  // Value found
        }
    }
    return -1;  // Value not found
}

//Reverse hash0
void invert_hash0(uint8_t k[8]) {

    uint8_t y = 0;
    uint64_t zTilde = 0;
    uint8_t p = 0;

    for (int i = 0; i < 8; i++) {
        y |= ((k[i] & 0x80) >> (7 - i)); // Recover the bit of y from the leftmost bit of k[i]
        pushbackSixBitByte(&zTilde, (k[i] & 0x7E) >> 1, i); // Recover the six bits of zTilde from the middle of k[i]

        if (g_debugMode > 0) printState("z~", zTilde);

        p |= ((k[i] & 0x01) << i);
    }

    if (g_debugMode > 0) PrintAndLogEx(INFO, "        y : %02x", y); // value of y (recovered 1 byte of the pre-image)
    // check if p is part of the array pi, if not invert it
    if (g_debugMode > 0) PrintAndLogEx(INFO, "        p : %02x", p); // value of p (at some point in the original hash0)

    int remainder = find_p_in_pi(p);
    if (remainder < 0) {
        p = ~p;
        remainder = find_p_in_pi(p);
    }

    if (g_debugMode > 0) PrintAndLogEx(INFO, "  p or ~p : %02x", p); // value of p (at some point in the original hash0)

    // find possible values of x that can return the same remainder
    uint8_t x_count = 0;
    uint8_t x_array[8];
    for (int x = 0x00; x <= 0xFF; x++) {
        if (x % 35 == remainder) {
            x_array[x_count] = x;
            x_count++;
        }
    }

    uint8_t pre_image_base[8] = {0};
    pre_image_base[1] = y;

    // calculate pre-images based on the potential values of x. Sshould we use pre-flip p and post flip p just in case?
    uint64_t zTil_img[8] = {0}; // 8 is the max size it'll have as per max number of X pre-images

    for (int img = 0; img < x_count; img++) { // for each potential value of x calculate a pre-image

        zTil_img[img] = zTilde;
        pre_image_base[0] = x_array[img];

        uint8_t pc = p; // redefine and reassociate it here or it'll keep changing through the loops
        if (x_array[img] & 1) { // Check if potential x7 is 1, if it is then invert p
            pc = ~p;
        }

        // calculate zTilde for the x preimage
        for (int i = 0; i < 8; i++) {

            uint8_t p_i = (pc >> i) & 0x1; // this is correct!
            uint8_t zTilde_i = getSixBitByte(zTilde, i) << 1;

            if (k[i] & 0x80) { // this checks the value of the first bit of the byte (value of y_i)
                if (p_i) {
                    zTilde_i--;
                }
                zTilde_i = ~zTilde_i; // flip the 6 bit string
            } else {
                zTilde_i |= p_i & 0x1;
            }

            pushbackSixBitByte(&zTil_img[img], zTilde_i >> 1, i);
        }

        if (g_debugMode > 0) {
            PrintAndLogEx(INFO, _YELLOW_("Testing Pre-Image Base: %s"), sprint_hex(pre_image_base, sizeof(pre_image_base)));
            PrintAndLogEx(DEBUG, "          | x| y|z0|z1|z2|z3|z4|z5|z6|z7|");
            printState("0|0|z~", zTil_img[img]); // we retrieve the values of z~
            PrintAndLogEx(INFO, "  p or ~p : %02x", pc); // value of p (at some point in the original hash0)
        }

        // reverse permute
        BitstreamIn_t p_in = { &pc, 8, 0 };
        uint8_t outbuffer_1[] = {0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t outbuffer_2[] = {0, 0, 0, 0, 0, 0, 0, 0};
        BitstreamOut_t out_1 = {outbuffer_1, 0, 0};
        BitstreamOut_t out_2 = {outbuffer_2, 0, 0};
        reverse_permute(&p_in, zTil_img[img], 0, &out_1, &out_2, false); // sort the bits

        // Shift z-values down onto the lower segment
        uint64_t zCaret_1 = x_bytes_to_num(outbuffer_1, sizeof(outbuffer_1));
        zCaret_1 >>= 16;
        uint64_t zCaret_2 = x_bytes_to_num(outbuffer_2, sizeof(outbuffer_2));
        zCaret_2 >>= 40;
        uint64_t zCaret = zCaret_1 | zCaret_2;
        if (g_debugMode > 0) printState("0|0|z^", zCaret);

        // fix the bits values
        uint8_t p_fix = 0x0F; // fix bits mask as the bits will be in 11110000 order
        BitstreamIn_t p_in_f = { &p_fix, 8, 0 };
        uint8_t outbuffer_f1[] = {0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t outbuffer_f2[] = {0, 0, 0, 0, 0, 0, 0, 0};
        BitstreamOut_t out_f1 = {outbuffer_f1, 0, 0};
        BitstreamOut_t out_f2 = {outbuffer_f2, 0, 0};
        reverse_permute(&p_in_f, zCaret, 0, &out_f1, &out_f2, true); // fixes the bits accordingly

        // Shift z-values down onto the lower segment
        uint64_t zCaret_fixed1 = x_bytes_to_num(outbuffer_f1, sizeof(outbuffer_f1));
        zCaret_fixed1 >>= 16;
        uint64_t zCaret_fixed2 = x_bytes_to_num(outbuffer_f2, sizeof(outbuffer_f2));
        zCaret_fixed2 >>= 40;

        uint64_t zCaret_fixed = zCaret_fixed1 | zCaret_fixed2;
        if (g_debugMode > 0) printState("0|0|z^", zCaret_fixed);

        uint64_t zP = reverse_check(zCaret_fixed);
        if (g_debugMode > 0) printState("0|0|z'", zP);

        // reverse the modulo transformation in the hash0 function for the six-bit chunks

        uint64_t c = 0;

        for (int n = 0; n < 4; n++) {
            uint8_t _zn = getSixBitByte(zP, n);
            uint8_t _zn4 = getSixBitByte(zP, n + 4);

            uint8_t zn = (_zn + (63 - 2 * n)) % (63 - n);
            uint8_t zn4 = (_zn4 + (64 - 2 * n)) % (64 - n);

            pushbackSixBitByte(&c, zn, n);
            pushbackSixBitByte(&c, zn4, n + 4);
        }

        // The Hydra: depending on their positions, values 0x00, 0x01, 0x02, 0x03, 0x3c, 0x3d, 0x3e, 0x3f can lead to additional pre-images.
        // When these values are present we need to generate additional pre-images if they have the same modulo as other values

        // Initialize an array of pointers to uint64_t (start with one value, initialized to 0)
        uint64_t *hydra_heads = (uint64_t *)calloc(sizeof(uint64_t), 1); // Start with one uint64_t
        if (hydra_heads == NULL) {
            PrintAndLogEx(WARNING, "Failed to allocate memory");
            return;
        }
        hydra_heads[0] = 0;  // Initialize first value to 0
        int heads_count = 1;  // Track number of forks

        // Iterate 4 times as per the original loop
        for (int n = 0; n < 8; n++) {

            uint8_t hydra_head = getSixBitByte(c, n);

            if (hydra_head <= (n % 4) || hydra_head >= 63 - (n % 4)) {

                // Create new forks by duplicating existing uint64_t values
                int new_head = heads_count * 2;

                // proper realloc pattern
                uint64_t *ptmp = (uint64_t *)realloc(hydra_heads, new_head * sizeof(uint64_t));
                if (ptmp == NULL) {
                    PrintAndLogEx(WARNING, "Failed to allocate memory");
                    free(hydra_heads);
                    return;
                }
                hydra_heads = ptmp;

                // Duplicate all current values and add the value to both original and new ones
                for (int i = 0; i < heads_count; i++) {

                    // Duplicate current value
                    hydra_heads[heads_count + i] = hydra_heads[i];
                    uint8_t small_hydra_head = 0;
                    uint8_t big_hydra_head = 0;
                    uint8_t hydra_lil_spawns[4] = {0x00, 0x01, 0x02, 0x03};
                    uint8_t hydra_big_spawns[4] = {0x3f, 0x3e, 0x3d, 0x3c};

                    if (hydra_head <= n % 4) { // check if is in the lower range

                        // replace with big spawn in one hydra and keep small in another
                        small_hydra_head = hydra_head;
                        for (int fh = 0; fh < 4; fh++) {
                            if (hydra_lil_spawns[fh] == hydra_head) {
                                big_hydra_head = hydra_big_spawns[fh];
                            }
                        }

                    } else if (hydra_head >= 63 - (n % 4)) { // or the higher range

                        // replace with small in one hydra and keep big in another
                        big_hydra_head = hydra_head;
                        for (int fh = 0; fh < 4; fh++) {
                            if (hydra_big_spawns[fh] == hydra_head) {
                                small_hydra_head = hydra_lil_spawns[fh];
                            }
                        }
                    }
                    // Add to both original and duplicate values
                    pushbackSixBitByte(&hydra_heads[i], big_hydra_head, n);
                    pushbackSixBitByte(&hydra_heads[heads_count + i], small_hydra_head, n);
                }
                // Update the count of total values
                heads_count = new_head;
            } else {
                // no hydra head spawns
                for (int i = 0; i < heads_count; i++) {
                    pushbackSixBitByte(&hydra_heads[i], hydra_head, n);;
                }
            }
        }

        for (int i = 0; i < heads_count; i++) {

            // restore the two most significant bytes (x and y)
            hydra_heads[i] |= ((uint64_t)x_array[img] << 56);
            hydra_heads[i] |= ((uint64_t)y << 48);

            if (g_debugMode > 0) {
                PrintAndLogEx(DEBUG, "          | x| y|z0|z1|z2|z3|z4|z5|z6|z7|");
                printState("origin_r1", hydra_heads[i]);
            }
            // reverse the swapZbalues function to get the original six-bit byte order
            uint64_t original_z = swapZvalues(hydra_heads[i]);

            if (g_debugMode > 0) {
                PrintAndLogEx(DEBUG, "          | x| y|z0|z1|z2|z3|z4|z5|z6|z7|");
                printState("origin_r2", original_z);
                PrintAndLogEx(INFO, "--------------------------");
            }
            // run pre-image through hash0
            uint8_t img_div_key[8] = {0};
            hash0(original_z, img_div_key); // commented to avoid log spam

            // verify result, if it matches add it to the list as a valid pre-image
            bool image_match = true;
            for (int v = 0; v < 8; v++) {

                // compare against input key k
                if (img_div_key[v] != k[v]) {
                    image_match = false;
                }

            }

            uint8_t des_pre_image[8] = {0};
            x_num_to_bytes(original_z, sizeof(original_z), des_pre_image);

            if (image_match) {
                PrintAndLogEx(INFO, "Pre-image......... " _YELLOW_("%s") " ( "_GREEN_("ok") " )", sprint_hex_inrow(des_pre_image, sizeof(des_pre_image)));
            } else {

                if (g_debugMode > 0) {
                    PrintAndLogEx(INFO, "Pre-image......... " _YELLOW_("%s") " ( "_RED_("invalid") " )", sprint_hex_inrow(des_pre_image, sizeof(des_pre_image)));
                }
            }
        }
        // Free allocated memory
        free(hydra_heads);
    }
}

/**
 * @brief Performs Elite-class key diversification
 * @param csn
 * @param key
 * @param div_key
 */
void diversifyKey(uint8_t *csn, uint8_t *key, uint8_t *div_key) {

    uint8_t crypted_csn[8] = {0};

    // Calculate DES(CSN, KEY)
    mbedtls_des_context ctx_enc;
    mbedtls_des_setkey_enc(&ctx_enc, key);
    mbedtls_des_crypt_ecb(&ctx_enc, csn, crypted_csn);
    mbedtls_des_free(&ctx_enc);

    //Calculate HASH0(DES))
    uint64_t c_csn = x_bytes_to_num(crypted_csn, sizeof(crypted_csn));
    hash0(c_csn, div_key);
}
/*
static void testPermute(void) {
    uint64_t x = 0;
    pushbackSixBitByte(&x, 0x00, 0);
    pushbackSixBitByte(&x, 0x01, 1);
    pushbackSixBitByte(&x, 0x02, 2);
    pushbackSixBitByte(&x, 0x03, 3);
    pushbackSixBitByte(&x, 0x04, 4);
    pushbackSixBitByte(&x, 0x05, 5);
    pushbackSixBitByte(&x, 0x06, 6);
    pushbackSixBitByte(&x, 0x07, 7);

    uint8_t mres[8] = { getSixBitByte(x, 0),
                        getSixBitByte(x, 1),
                        getSixBitByte(x, 2),
                        getSixBitByte(x, 3),
                        getSixBitByte(x, 4),
                        getSixBitByte(x, 5),
                        getSixBitByte(x, 6),
                        getSixBitByte(x, 7)
                      };
    printarr("input_perm", mres, 8);

    uint8_t p = ~pi[0];
    BitstreamIn_t p_in = { &p, 8, 0 };
    uint8_t outbuffer[] = {0, 0, 0, 0, 0, 0, 0, 0};
    BitstreamOut_t out = {outbuffer, 0, 0};

    permute(&p_in, x, 0, 4, &out);

    uint64_t permuted = x_bytes_to_num(outbuffer, 8);
    // PrintAndLogEx(NORMAL, "zTilde 0x%"PRIX64, zTilde);
    permuted >>= 16;

    uint8_t res[8] = { getSixBitByte(permuted, 0),
                       getSixBitByte(permuted, 1),
                       getSixBitByte(permuted, 2),
                       getSixBitByte(permuted, 3),
                       getSixBitByte(permuted, 4),
                       getSixBitByte(permuted, 5),
                       getSixBitByte(permuted, 6),
                       getSixBitByte(permuted, 7)
                     };
    printarr("permuted", res, 8);
}
*/
// These testcases are
// { UID , TEMP_KEY, DIV_KEY} using the specific key
typedef struct {
    uint8_t uid[8];
    uint8_t t_key[8];
    uint8_t div_key[8];
} testcase_t;

static int testDES(uint8_t *key, testcase_t testcase) {
    uint8_t des_encrypted_csn[8] = {0};
    uint8_t decrypted[8] = {0};
    uint8_t div_key[8] = {0};

    mbedtls_des_context ctx_enc;
    mbedtls_des_context ctx_dec;

    mbedtls_des_setkey_enc(&ctx_enc, key);
    mbedtls_des_setkey_dec(&ctx_dec, key);

    int retval = mbedtls_des_crypt_ecb(&ctx_enc, testcase.uid, des_encrypted_csn);
    retval |= mbedtls_des_crypt_ecb(&ctx_dec, des_encrypted_csn, decrypted);

    mbedtls_des_free(&ctx_enc);
    mbedtls_des_free(&ctx_dec);

    if (memcmp(testcase.uid, decrypted, 8) != 0) {
        //Decryption fail
        PrintAndLogEx(FAILED, "Encryption <-> Decryption FAIL");
        printarr("    input", testcase.uid, 8);
        printarr("    decrypted", decrypted, 8);
        retval = 1;
    }

    if (memcmp(des_encrypted_csn, testcase.t_key, 8) != 0) {
        //Encryption fail
        PrintAndLogEx(FAILED, "Encryption != Expected result");
        printarr("    output", des_encrypted_csn, 8);
        printarr("    expected", testcase.t_key, 8);
        retval = 1;
    }
    uint64_t crypted_csn = x_bytes_to_num(des_encrypted_csn, 8);
    hash0(crypted_csn, div_key);

    if (memcmp(div_key, testcase.div_key, 8) != 0) {
        //Key diversification fail
        PrintAndLogEx(FAILED, "Div key != expected result");
        printarr("  csn   ", testcase.uid, 8);
        printarr("{csn}   ", des_encrypted_csn, 8);
        printarr("hash0   ", div_key, 8);
        printarr("    expected", testcase.div_key, 8);
        retval = 1;
    }
    return retval;
}
static bool des_getParityBitFromKey(uint8_t key) {
    // The top 7 bits is used
    bool parity = ((key & 0x80) >> 7)
                  ^ ((key & 0x40) >> 6)
                  ^ ((key & 0x20) >> 5)
                  ^ ((key & 0x10) >> 4)
                  ^ ((key & 0x08) >> 3)
                  ^ ((key & 0x04) >> 2)
                  ^ ((key & 0x02) >> 1);
    return !parity;
}

static void des_checkParity(uint8_t *key) {
    int fails = 0;
    for (uint8_t i = 0; i < 8; i++) {
        bool parity = des_getParityBitFromKey(key[i]);
        if (parity != (key[i] & 0x1)) {
            fails++;
            PrintAndLogEx(FAILED, "parity1 fail, byte %d [%02x] was %d, should be %d", i, key[i], (key[i] & 0x1), parity);
        }
    }

    if (fails) {
        PrintAndLogEx(FAILED, "parity fails...  " _RED_("%d"), fails);
    } else {
        PrintAndLogEx(SUCCESS, "    Key syntax is with parity bits inside each byte (%s)", _GREEN_("ok"));
    }
}

testcase_t testcases[] = {

    {{0x8B, 0xAC, 0x60, 0x1F, 0x53, 0xB8, 0xED, 0x11}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0xAE, 0x51, 0xE5, 0x62, 0xE7, 0x9A, 0x99, 0x39}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}, {0x04, 0x02, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x9B, 0x21, 0xE4, 0x31, 0x6A, 0x00, 0x29, 0x62}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}, {0x06, 0x04, 0x02, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x65, 0x24, 0x0C, 0x41, 0x4F, 0xC2, 0x21, 0x93}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}, {0x0A, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x7F, 0xEB, 0xAE, 0x93, 0xE5, 0x30, 0x08, 0xBD}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08}, {0x12, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x49, 0x7B, 0x70, 0x74, 0x9B, 0x35, 0x1B, 0x83}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10}, {0x22, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x02, 0x3C, 0x15, 0x6B, 0xED, 0xA5, 0x64, 0x6C}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20}, {0x42, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0xE8, 0x37, 0xE0, 0xE2, 0xC6, 0x45, 0x24, 0xF3}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40}, {0x02, 0x06, 0x04, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0xAB, 0xBD, 0x30, 0x05, 0x29, 0xC8, 0xF7, 0x12}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, {0x02, 0x08, 0x06, 0x04, 0x01, 0x03, 0x05, 0x07}},
    {{0x17, 0xE8, 0x97, 0xF0, 0x99, 0xB6, 0x79, 0x31}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00}, {0x02, 0x0C, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x49, 0xA4, 0xF0, 0x8F, 0x5F, 0x96, 0x83, 0x16}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00}, {0x02, 0x14, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x60, 0xF5, 0x7E, 0x54, 0xAA, 0x41, 0x83, 0xD4}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00}, {0x02, 0x24, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x1D, 0xF6, 0x3B, 0x6B, 0x85, 0x55, 0xF0, 0x4B}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00}, {0x02, 0x44, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x1F, 0xDC, 0x95, 0x1A, 0xEA, 0x6B, 0x4B, 0xB4}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00}, {0x02, 0x04, 0x08, 0x06, 0x01, 0x03, 0x05, 0x07}},
    {{0xEC, 0x93, 0x72, 0xF0, 0x3B, 0xA9, 0xF5, 0x0B}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00}, {0x02, 0x04, 0x0A, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0xDE, 0x57, 0x5C, 0xBE, 0x2D, 0x55, 0x03, 0x12}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00}, {0x02, 0x04, 0x0E, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x1E, 0xD2, 0xB5, 0xCE, 0x90, 0xC9, 0xC1, 0xCC}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00}, {0x02, 0x04, 0x16, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0xD8, 0x65, 0x96, 0x4E, 0xE7, 0x74, 0x99, 0xB8}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}, {0x02, 0x04, 0x26, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0xE3, 0x7A, 0x29, 0x83, 0x31, 0xD5, 0x3A, 0x54}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00}, {0x02, 0x04, 0x46, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x3A, 0xB5, 0x1A, 0x34, 0x34, 0x25, 0x12, 0xF0}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x0A, 0x01, 0x03, 0x05, 0x07}},
    {{0xF2, 0x88, 0xEE, 0x6F, 0x70, 0x6F, 0xC2, 0x52}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x0C, 0x01, 0x03, 0x05, 0x07}},
    {{0x76, 0xEF, 0xEB, 0x80, 0x52, 0x43, 0x83, 0x57}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x10, 0x01, 0x03, 0x05, 0x07}},
    {{0x1C, 0x09, 0x8E, 0x3B, 0x23, 0x23, 0x52, 0xB5}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x18, 0x01, 0x03, 0x05, 0x07}},
    {{0xA9, 0x13, 0xA2, 0xBE, 0xCF, 0x1A, 0xC4, 0x9A}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x28, 0x01, 0x03, 0x05, 0x07}},
    {{0x25, 0x56, 0x4B, 0xB0, 0xC8, 0x2A, 0xD4, 0x27}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x48, 0x01, 0x03, 0x05, 0x07}},
    {{0xB1, 0x04, 0x57, 0x3F, 0xA7, 0x16, 0x62, 0xD4}, {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x03, 0x01, 0x05, 0x07}},
    {{0x45, 0x46, 0xED, 0xCC, 0xE7, 0xD3, 0x8E, 0xA3}, {0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x05, 0x03, 0x01, 0x07}},
    {{0x22, 0x6D, 0xB5, 0x35, 0xE0, 0x5A, 0xE0, 0x90}, {0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x09, 0x03, 0x05, 0x07}},
    {{0xB8, 0xF5, 0xE5, 0x44, 0xC5, 0x98, 0x4A, 0xBD}, {0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x11, 0x03, 0x05, 0x07}},
    {{0xAC, 0x78, 0x0A, 0x23, 0x9E, 0xF6, 0xBC, 0xA0}, {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x21, 0x03, 0x05, 0x07}},
    {{0x46, 0x6B, 0x2D, 0x70, 0x41, 0x17, 0xBF, 0x3D}, {0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x41, 0x03, 0x05, 0x07}},
    {{0x64, 0x44, 0x24, 0x71, 0xA2, 0x56, 0xDF, 0xB5}, {0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x05, 0x03, 0x07}},
    {{0xC4, 0x00, 0x52, 0x24, 0xA2, 0xD6, 0x16, 0x7A}, {0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x07, 0x05, 0x03}},
    {{0xD8, 0x4A, 0x80, 0x1E, 0x95, 0x5B, 0x70, 0xC4}, {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x0B, 0x05, 0x07}},
    {{0x08, 0x56, 0x6E, 0xB5, 0x64, 0xD6, 0x47, 0x4E}, {0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x13, 0x05, 0x07}},
    {{0x41, 0x6F, 0xBA, 0xA4, 0xEB, 0xAE, 0xA0, 0x55}, {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x23, 0x05, 0x07}},
    {{0x62, 0x9D, 0xDE, 0x72, 0x84, 0x4A, 0x53, 0xD5}, {0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x43, 0x05, 0x07}},
    {{0x39, 0xD3, 0x2B, 0x66, 0xB8, 0x08, 0x40, 0x2E}, {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x07, 0x05}},
    {{0xAF, 0x67, 0xA9, 0x18, 0x57, 0x21, 0xAF, 0x8D}, {0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x09, 0x07}},
    {{0x34, 0xBC, 0x9D, 0xBC, 0xC4, 0xC2, 0x3B, 0xC8}, {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x0D, 0x07}},
    {{0xB6, 0x50, 0xF9, 0x81, 0xF6, 0xBF, 0x90, 0x3C}, {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x15, 0x07}},
    {{0x71, 0x41, 0x93, 0xA1, 0x59, 0x81, 0xA5, 0x52}, {0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x25, 0x07}},
    {{0x6B, 0x00, 0xBD, 0x74, 0x1C, 0x3C, 0xE0, 0x1A}, {0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x45, 0x07}},
    {{0x76, 0xFD, 0x0B, 0xD0, 0x41, 0xD2, 0x82, 0x5D}, {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x09}},
    {{0xC6, 0x3A, 0x1C, 0x25, 0x63, 0x5A, 0x2F, 0x0E}, {0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x0B}},
    {{0xD9, 0x0E, 0xD7, 0x30, 0xE2, 0xAD, 0xA9, 0x87}, {0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x0F}},
    {{0x6B, 0x81, 0xC6, 0xD1, 0x05, 0x09, 0x87, 0x1E}, {0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x17}},
    {{0xB4, 0xA7, 0x1E, 0x02, 0x54, 0x37, 0x43, 0x35}, {0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x27}},
    {{0x45, 0x14, 0x7C, 0x7F, 0xE0, 0xDE, 0x09, 0x65}, {0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x47}},
    {{0x78, 0xB0, 0xF5, 0x20, 0x8B, 0x7D, 0xF3, 0xDD}, {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0xFE, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x88, 0xB3, 0x3C, 0xE1, 0xF7, 0x87, 0x42, 0xA1}, {0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0xFC, 0x06, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x11, 0x2F, 0xB2, 0xF7, 0xE2, 0xB2, 0x4F, 0x6E}, {0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0xFA, 0x08, 0x01, 0x03, 0x05, 0x07}},
    {{0x25, 0x56, 0x4E, 0xC6, 0xEB, 0x2D, 0x74, 0x5B}, {0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0xF8, 0x01, 0x03, 0x05, 0x07}},
    {{0x7E, 0x98, 0x37, 0xF9, 0x80, 0x8F, 0x09, 0x82}, {0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0xFF, 0x03, 0x05, 0x07}},
    {{0xF9, 0xB5, 0x62, 0x3B, 0xD8, 0x7B, 0x3C, 0x3F}, {0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0xFD, 0x05, 0x07}},
    {{0x29, 0xC5, 0x2B, 0xFA, 0xD1, 0xFC, 0x5C, 0xC7}, {0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0xFB, 0x07}},
    {{0xC1, 0xA3, 0x09, 0x71, 0xBD, 0x8E, 0xAF, 0x2F}, {0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x06, 0x08, 0x01, 0x03, 0x05, 0xF9}},
    {{0xB6, 0xDD, 0xD1, 0xAD, 0xAA, 0x15, 0x6F, 0x29}, {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x01, 0x03, 0x05, 0x02, 0x07, 0x04, 0x06, 0x08}},
    {{0x65, 0x34, 0x03, 0x19, 0x17, 0xB3, 0xA3, 0x96}, {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x01, 0x06, 0x08, 0x03, 0x05, 0x07}},
    {{0xF9, 0x38, 0x43, 0x56, 0x52, 0xE5, 0xB1, 0xA9}, {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x01, 0x02, 0x04, 0x06, 0x08, 0x03, 0x05, 0x07}},

    {{0xA4, 0xA0, 0xAF, 0xDA, 0x48, 0xB0, 0xA1, 0x10}, {0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x01, 0x02, 0x04, 0x06, 0x03, 0x08, 0x05, 0x07}},
    {{0x55, 0x15, 0x8A, 0x0D, 0x48, 0x29, 0x01, 0xD8}, {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x01, 0x06, 0x03, 0x05, 0x08, 0x07}},
    {{0xC4, 0x81, 0x96, 0x7D, 0xA3, 0xB7, 0x73, 0x50}, {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x01, 0x02, 0x03, 0x05, 0x04, 0x06, 0x08, 0x07}},
    {{0x36, 0x73, 0xDF, 0xC1, 0x1B, 0x98, 0xA8, 0x1D}, {0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x07}},
    {{0xCE, 0xE0, 0xB3, 0x1B, 0x41, 0xEB, 0x15, 0x12}, {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x01, 0x02, 0x03, 0x04, 0x06, 0x05, 0x08, 0x07}},
    {{0}, {0}, {0}}
};

static int testKeyDiversificationWithMasterkeyTestcases(uint8_t *key) {
    int i, error = 0;
    uint8_t empty[8] = {0};

    PrintAndLogEx(INFO, "Testing encryption/decryption...");

    for (i = 0; memcmp(testcases + i, empty, 8); i++) {
        error += testDES(key, testcases[i]);
    }

    if (error) {
        PrintAndLogEx(FAILED, "%d errors occurred, %d testcases ( %s )", error, i, _RED_("fail"));
    } else {
        PrintAndLogEx(SUCCESS, "    Hashing seems to work, " _YELLOW_("%d") " testcases ( %s )", i, _GREEN_("ok"));
    }
    return error;
}

static int testCryptedCSN(uint64_t crypted_csn, uint64_t expected) {

    uint8_t result[8] = {0};
    uint64_t crypted_csn_swapped = swapZvalues(crypted_csn);
    hash0(crypted_csn, result);
    uint64_t resultbyte = x_bytes_to_num(result, 8);

    PrintAndLogEx(DEBUG, "");
    PrintAndLogEx(DEBUG, "    {csn}      %"PRIx64, crypted_csn);
    PrintAndLogEx(DEBUG, "    {csn-revz} %"PRIx64, crypted_csn_swapped);
    PrintAndLogEx(DEBUG, "    hash0      %"PRIx64 "   (%s)", resultbyte, (resultbyte == expected) ? _GREEN_("ok") : _RED_("fail"));

    if (resultbyte != expected) {
        PrintAndLogEx(DEBUG, "    expected       " _YELLOW_("%"PRIx64),  expected);
        return PM3_ESOFT;
    }
    return PM3_SUCCESS;
}

static int testDES2(uint8_t *key, uint64_t csn, uint64_t expected) {
    uint8_t result[8] = {0};
    uint8_t input[8] = {0};

    PrintAndLogEx(DEBUG, "   csn      %"PRIx64, csn);
    x_num_to_bytes(csn, 8, input);

    mbedtls_des_context ctx_enc;
    mbedtls_des_setkey_enc(&ctx_enc, key);
    mbedtls_des_crypt_ecb(&ctx_enc, input, result);
    mbedtls_des_free(&ctx_enc);

    uint64_t crypt_csn = x_bytes_to_num(result, 8);

    PrintAndLogEx(DEBUG, "   {csn}    %"PRIx64, crypt_csn);
    PrintAndLogEx(DEBUG, "   expected %"PRIx64 "    (%s)", expected, (expected == crypt_csn) ? _GREEN_("ok") : _RED_("fail"));

    if (expected != crypt_csn) {
        return PM3_ESOFT;
    }
    return PM3_SUCCESS;
}

/**
 * These testcases come from http://www.proxmark.org/forum/viewtopic.php?pid=10977#p10977
 * @brief doTestsWithKnownInputs
 * @return
 */
static int doTestsWithKnownInputs(void) {
    // KSel from http://www.proxmark.org/forum/viewtopic.php?pid=10977#p10977
    PrintAndLogEx(INFO, "Testing DES encryption... ");
    uint8_t key[8] = {0x6c, 0x8d, 0x44, 0xf9, 0x2a, 0x2d, 0x01, 0xbf};

    testDES2(key, 0xbbbbaaaabbbbeeee, 0xd6ad3ca619659e6b);

    PrintAndLogEx(INFO, "Testing hashing algorithm... ");

    int res = PM3_SUCCESS;
    res += testCryptedCSN(0x0102030405060708, 0x0bdd6512073c460a);
    res += testCryptedCSN(0x1020304050607080, 0x0208211405f3381f);
    res += testCryptedCSN(0x1122334455667788, 0x2bee256d40ac1f3a);
    res += testCryptedCSN(0xabcdabcdabcdabcd, 0xa91c9ec66f7da592);
    res += testCryptedCSN(0xbcdabcdabcdabcda, 0x79ca5796a474e19b);
    res += testCryptedCSN(0xcdabcdabcdabcdab, 0xa8901b9f7ec76da4);
    res += testCryptedCSN(0xdabcdabcdabcdabc, 0x357aa8e0979a5b8d);
    res += testCryptedCSN(0x21ba6565071f9299, 0x34e80f88d5cf39ea);
    res += testCryptedCSN(0x14e2adfc5bb7e134, 0x6ac90c6508bd9ea3);

    if (res != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "%d res occurred " _YELLOW_("9") " testcases ( %s )", res, _RED_("fail"));
        res = PM3_ESOFT;
    } else {
        PrintAndLogEx(SUCCESS, "    Hashing seems to work " _YELLOW_("9") " testcases ( %s )", _GREEN_("ok"));
        res = PM3_SUCCESS;
    }
    return res;
}

int doKeyTests(void) {

    uint8_t key[8] = { 0xAE, 0xA6, 0x84, 0xA6, 0xDA, 0xB2, 0x32, 0x78 };
    uint8_t parity[8] = {0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x00, 0x01};

    for (int i = 0; i < 8; i++) {
        key[i] += parity[i];
    }

    PrintAndLogEx(SUCCESS, "Checking key parity...");
    des_checkParity(key);

    // Test hashing functions
    testKeyDiversificationWithMasterkeyTestcases(key);
    PrintAndLogEx(INFO, "Testing key diversification with non-sensitive keys...");
    return doTestsWithKnownInputs();
}

/**

void checkParity2(uint8_t* key) {

    uint8_t stored_parity = key[7];
    PrintAndLogEx(NORMAL, "Parity byte: 0x%02x", stored_parity);
    int i, byte, fails = 0;
    BitstreamIn_t bits = {key, 56, 0};
    bool parity = 0;

    for (i = 0; i  < 56; i++) {

        if ( i > 0 && i % 7 == 0){
            parity = !parity;
            bool pbit = stored_parity & (0x80 >> (byte));
            if (parity != pbit) {
                PrintAndLogEx(NORMAL, "parity2 fail byte %d, should be %d, was %d", (i / 7), parity, pbit);
                fails++;
            }
            parity =0 ;
            byte = i / 7;
        }
        parity = parity ^ headBit(&bits);
    }
    if (fails) {
        PrintAndLogEx(FAILED, "parity2 fails: %d", fails);
    } else {
        PrintAndLogEx(INFO, "Key syntax is with parity bits grouped in the last byte!");
    }
}

void modifyKey_put_parity_last(uint8_t * key, uint8_t* output) {

    uint8_t paritybits = 0;
    bool parity =0;
    BitstreamOut_t out = { output, 0, 0};
    unsigned int bbyte, bbit;
    for (bbyte = 0; bbyte <8; bbyte++ ) {
        for(bbit = 0; bbit < 7; bbit++) {
            bool bit = *(key + bbyte) & (1 << (7 - bbit));
            pushBit(&out, bit);
            parity ^= bit;
        }
        bool paritybit = *(key + bbyte) & 1;
        paritybits |= paritybit << (7 - bbyte);
        parity = 0;

    }
    output[7] = paritybits;
    PrintAndLogEx(INFO, "Parity byte: %02x", paritybits);
}

 * @brief Modifies a key with parity bits last, so that it is formed with parity
 *    bits inside each byte
 * @param key
 * @param output

void modifyKey_put_parity_allover(uint8_t * key, uint8_t* output) {
    bool parity =0;
    BitstreamOut_t out = {output, 0, 0};
    BitstreamIn_t in = {key, 0, 0};
    unsigned int bbyte, bbit;
    for (bbit = 0; bbit < 56; bbit++) {
        if (bbit > 0 && bbit % 7 == 0) {
            pushBit(&out, !parity);
            parity = 0;
        }
        bool bit = headBit(&in);
        pushBit(&out, bit);
        parity ^= bit;
    }
    pushBit(&out, !parity);

    if (des_key_check_key_parity(output))
        PrintAndLogEx(FAILED, "modifyKey_put_parity_allover fail, DES key invalid parity!");
}
*/



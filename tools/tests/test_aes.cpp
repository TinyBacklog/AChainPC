// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the AES crypto extension instructions.
//
// Two levels of check. First the tables and each instruction against a
// reference derived from FIPS-197 from first principles. Then, decisively, a
// full AES-128 block encryption built out of AESE and AESMC, compared against
// OpenSSL - which is what the guest actually relies on being right.
#include <cassert>
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

static std::string RuntimeSection() {
    const std::string rt = suyu::recomp::RuntimeC();
    const size_t a = rt.find("static const uint8_t _aes_sbox[256]");
    assert(a != std::string::npos && "AES helpers missing from RuntimeC()");
    const size_t b = rt.find("/* \xe2\x94\x80\xe2\x94\x80 Reciprocal estimates", a);
    assert(b != std::string::npos && "end marker missing");
    return rt.substr(a, b - a);
}

int main() {
    std::cout << R"C(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
typedef struct GuestContext { uint64_t vreg[32][2]; } GuestContext;
)C";
    std::cout << RuntimeSection();
    std::cout << R"C(
/* Referencia independente: deriva o S-box do inverso multiplicativo em vez de
   o ler da tabela acima, para que um erro de transcricao apareca. */
static uint8_t r_gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0; int i;
    for (i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        { const uint8_t hi = a & 0x80; a = (uint8_t)(a << 1); if (hi) a ^= 0x1b; }
        b = (uint8_t)(b >> 1);
    }
    return p;
}
static uint8_t r_sbox[256], r_isbox[256];
static void r_init(void) {
    uint8_t inv[256]; int a, b, i, k;
    inv[0] = 0;
    for (a = 1; a < 256; ++a)
        for (b = 1; b < 256; ++b)
            if (r_gmul((uint8_t)a, (uint8_t)b) == 1) { inv[a] = (uint8_t)b; break; }
    for (i = 0; i < 256; ++i) {
        uint8_t x = inv[i], s = x;
        for (k = 0; k < 4; ++k) { x = (uint8_t)((x << 1) | (x >> 7)); s ^= x; }
        r_sbox[i] = (uint8_t)(s ^ 0x63);
    }
    for (i = 0; i < 256; ++i) r_isbox[r_sbox[i]] = (uint8_t)i;
}
static void r_shiftrows(uint8_t* o, const uint8_t* v, int inverse) {
    int j;
    for (j = 0; j < 16; ++j) {
        const int r = j & 3, c = j >> 2;
        const int src = inverse ? ((c - r) & 3) : ((c + r) & 3);
        o[j] = v[r + 4 * src];
    }
}
static void r_mix(uint8_t* o, const uint8_t* v, int inverse) {
    static const uint8_t fwd[4] = {2, 3, 1, 1}, rev[4] = {14, 11, 13, 9};
    const uint8_t* k = inverse ? rev : fwd;
    int c, r, i;
    for (c = 0; c < 4; ++c)
        for (r = 0; r < 4; ++r) {
            uint8_t acc = 0;
            for (i = 0; i < 4; ++i) acc ^= r_gmul(v[((r + i) & 3) + 4 * c], k[i]);
            o[r + 4 * c] = acc;
        }
}
)C";

    unsigned slot = 0;
    std::string dispatch;
    auto emit = [&](uint32_t insn) {
        std::string body;
        if (!suyu::recomp::Translate(insn, 0, body) ||
            body.find("recomp_unhandled") != std::string::npos) {
            std::cerr << "not translated: " << std::hex << insn << '\n';
            std::exit(1);
        }
        std::cout << "static void f" << slot << "(GuestContext* c){" << body << "}\n";
        dispatch += "f" + std::to_string(slot) + ",";
        ++slot;
    };
    // Rd = 0, Rn = 1 para as quatro instrucoes, na ordem AESE, AESD, AESMC, AESIMC.
    for (unsigned k = 0; k < 4; ++k)
        emit(0x4E284800u | (k << 12) | (1u << 5) | 0u);

    std::cout << "static void (*functions[])(GuestContext*) = {" << dispatch << "};\n";
    std::cout << R"C(
static int checks = 0;

/* AES-128 construido a partir das instrucoes, como o codigo guest faz:
   nove rondas de AESE+AESMC com a chave, mais uma ronda final so com AESE. */
static void aes128_via_insns(const uint8_t* key, const uint8_t* in, uint8_t* out) {
    uint8_t rk[11][16];
    int r, i;
    memcpy(rk[0], key, 16);
    for (r = 1; r <= 10; ++r) {
        static const uint8_t rcon[11] = {0,1,2,4,8,16,32,64,128,27,54};
        uint8_t t[4];
        for (i = 0; i < 4; ++i) t[i] = r_sbox[rk[r-1][12 + ((i + 1) & 3)]];
        t[0] ^= rcon[r];
        for (i = 0; i < 4; ++i) rk[r][i] = rk[r-1][i] ^ t[i];
        for (i = 4; i < 16; ++i) rk[r][i] = rk[r-1][i] ^ rk[r][i-4];
    }
    {
        GuestContext c;
        memset(&c, 0, sizeof c);
        memcpy(c.vreg[0], in, 16);
        for (r = 0; r < 9; ++r) {
            memcpy(c.vreg[1], rk[r], 16);
            functions[0](&c);                 /* AESE  v0, v1 */
            { GuestContext t2 = c;            /* AESMC v0, v0 */
              memcpy(t2.vreg[1], c.vreg[0], 16);
              memcpy(c.vreg[1], c.vreg[0], 16);
              (void)t2; }
            functions[2](&c);                 /* usa Rn=1, que acabou de receber v0 */
        }
        memcpy(c.vreg[1], rk[9], 16);
        functions[0](&c);                     /* ultima AESE, sem MixColumns */
        {
            uint8_t st[16]; int j;
            memcpy(st, c.vreg[0], 16);
            for (j = 0; j < 16; ++j) st[j] ^= rk[10][j];
            memcpy(out, st, 16);
        }
    }
}

int main(void) {
    uint8_t a[16], b[16], want[16], got[16];
    int i, t;
    r_init();

    /* As tabelas do runtime batem com a derivacao independente. */
    for (i = 0; i < 256; ++i) {
        if (_aes_sbox[i] != r_sbox[i]) { printf("FAIL sbox %d\n", i); return 1; }
        if (_aes_isbox[i] != r_isbox[i]) { printf("FAIL isbox %d\n", i); return 1; }
        checks += 2;
    }

    /* Cada instrucao contra a referencia, com varios operandos. */
    for (t = 0; t < 24; ++t) {
        GuestContext c;
        uint8_t x[16], y[16], tmp[16];
        for (i = 0; i < 16; ++i) { a[i] = (uint8_t)(t * 31 + i * 7 + 1);
                                   b[i] = (uint8_t)(t * 17 + i * 13 + 200); }
        /* AESE */
        memset(&c, 0, sizeof c);
        memcpy(c.vreg[0], a, 16); memcpy(c.vreg[1], b, 16);
        functions[0](&c);
        for (i = 0; i < 16; ++i) x[i] = (uint8_t)(a[i] ^ b[i]);
        r_shiftrows(tmp, x, 0);
        for (i = 0; i < 16; ++i) want[i] = r_sbox[tmp[i]];
        memcpy(got, c.vreg[0], 16);
        if (memcmp(got, want, 16)) { printf("FAIL aese t=%d\n", t); return 1; }
        ++checks;
        /* AESD */
        memset(&c, 0, sizeof c);
        memcpy(c.vreg[0], a, 16); memcpy(c.vreg[1], b, 16);
        functions[1](&c);
        r_shiftrows(tmp, x, 1);
        for (i = 0; i < 16; ++i) want[i] = r_isbox[tmp[i]];
        memcpy(got, c.vreg[0], 16);
        if (memcmp(got, want, 16)) { printf("FAIL aesd t=%d\n", t); return 1; }
        ++checks;
        /* AESMC e AESIMC */
        memset(&c, 0, sizeof c);
        memcpy(c.vreg[1], a, 16);
        functions[2](&c);
        r_mix(want, a, 0);
        memcpy(got, c.vreg[0], 16);
        if (memcmp(got, want, 16)) { printf("FAIL aesmc t=%d\n", t); return 1; }
        ++checks;
        memset(&c, 0, sizeof c);
        memcpy(c.vreg[1], a, 16);
        functions[3](&c);
        r_mix(want, a, 1);
        memcpy(got, c.vreg[0], 16);
        if (memcmp(got, want, 16)) { printf("FAIL aesimc t=%d\n", t); return 1; }
        ++checks;
        /* AESIMC desfaz AESMC. */
        memset(&c, 0, sizeof c);
        memcpy(c.vreg[1], a, 16);
        functions[2](&c);
        memcpy(y, c.vreg[0], 16);
        memset(&c, 0, sizeof c);
        memcpy(c.vreg[1], y, 16);
        functions[3](&c);
        memcpy(got, c.vreg[0], 16);
        if (memcmp(got, a, 16)) { printf("FAIL mixcolumns nao e inversa t=%d\n", t); return 1; }
        ++checks;
    }

    /* A prova que conta: uma cifragem AES-128 completa montada com as
       instrucoes, contra o OpenSSL. */
    for (t = 0; t < 8; ++t) {
        uint8_t key[16], in[16], ossl[16] = {0};
        int len = 0;
        EVP_CIPHER_CTX* ctx;
        for (i = 0; i < 16; ++i) { key[i] = (uint8_t)(t * 41 + i * 3 + 5);
                                   in[i]  = (uint8_t)(t * 23 + i * 11 + 77); }
        ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        EVP_EncryptUpdate(ctx, ossl, &len, in, 16);
        EVP_CIPHER_CTX_free(ctx);
        aes128_via_insns(key, in, got);
        if (memcmp(got, ossl, 16)) {
            printf("FAIL aes128 t=%d\n  insns:", t);
            for (i = 0; i < 16; ++i) printf(" %02x", got[i]);
            printf("\n  ossl :");
            for (i = 0; i < 16; ++i) printf(" %02x", ossl[i]);
            printf("\n");
            return 1;
        }
        ++checks;
    }
    if (sizeof functions / sizeof functions[0] != 4) return 1;
    printf("%d AES comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}

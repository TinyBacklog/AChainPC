// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the SHA-256 crypto extension instructions.
//
// Ao contrario do SHA-1, o dynarmic nao tem referencia portatil para estas
// quatro: o backend x64 delega no sha256rnds2 do processador. A semantica foi
// derivada da funcao de ronda do FIPS-180-4, por isso a unica prova aceitavel
// e montar um SHA-256 completo com as instrucoes e comparar com o OpenSSL.
//
// As constantes K nao sao transcritas: o programa gerado calcula-as com a
// definicao do FIPS (parte fraccionaria da raiz cubica dos 64 primeiros
// primos) em aritmetica inteira exacta.
#include <cassert>
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

static std::string RuntimeSection() {
    const std::string rt = suyu::recomp::RuntimeC();
    const size_t a = rt.find("static uint32_t _sha_ror32(uint32_t x, unsigned n)");
    assert(a != std::string::npos && "SHA helpers missing from RuntimeC()");
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
    emit(0x5E004000u | (2u << 16) | (1u << 5) | 0u);   // 0: SHA256H   Vd=0,Vn=1,Vm=2
    emit(0x5E005000u | (2u << 16) | (1u << 5) | 0u);   // 1: SHA256H2  Vd=0,Vn=1,Vm=2
    emit(0x5E006000u | (2u << 16) | (1u << 5) | 0u);   // 2: SHA256SU1 Vd=0,Vn=1,Vm=2
    emit(0x5E282800u | (1u << 5) | 0u);                // 3: SHA256SU0 Vd=0,Vn=1

    std::cout << "static void (*functions[])(GuestContext*) = {" << dispatch << "};\n";
    std::cout << R"C(
static int checks = 0;

static void run3(int idx, uint32_t* d, const uint32_t* n, const uint32_t* m) {
    GuestContext c;
    memset(&c, 0, sizeof c);
    memcpy(c.vreg[0], d, 16);
    memcpy(c.vreg[1], n, 16);
    memcpy(c.vreg[2], m, 16);
    functions[idx](&c);
    memcpy(d, c.vreg[0], 16);
}
static void run2(int idx, uint32_t* d, const uint32_t* n) {
    GuestContext c;
    memset(&c, 0, sizeof c);
    memcpy(c.vreg[0], d, 16);
    memcpy(c.vreg[1], n, 16);
    functions[idx](&c);
    memcpy(d, c.vreg[0], 16);
}

/* ── Constantes K, derivadas da definicao do FIPS ──
   K[t] = 32 bits de cima da parte fraccionaria da raiz cubica do primo t.
   Raiz cubica inteira de p * 2^96 = cbrt(p) * 2^32; o resto modulo 2^32 e
   exactamente a parte fraccionaria pedida. Nada aqui e transcrito. */
static uint32_t K[64];
static void derive_K(void) {
    int count = 0, p;
    for (p = 2; count < 64; ++p) {
        int d, prime = 1;
        for (d = 2; d * d <= p; ++d)
            if (p % d == 0) { prime = 0; break; }
        if (!prime) continue;
        {
            __uint128_t target = (__uint128_t)p << 96;
            __uint128_t lo = 0, hi = (__uint128_t)1 << 36;
            while (hi - lo > 1) {            /* maior c com c^3 <= p * 2^96 */
                __uint128_t mid = lo + (hi - lo) / 2;
                if (mid * mid * mid <= target) lo = mid; else hi = mid;
            }
            K[count] = (uint32_t)(lo & 0xffffffffu);
        }
        ++count;
    }
}

/* Modelo de referencia independente para o agendamento de mensagem, escrito
   direto do FIPS e nao a partir dos ajudantes do tradutor. */
static uint32_t rr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static uint32_t ref_s0(uint32_t x) { return rr(x, 7) ^ rr(x, 18) ^ (x >> 3); }
static uint32_t ref_s1(uint32_t x) { return rr(x, 17) ^ rr(x, 19) ^ (x >> 10); }

/* SHA-256 de um bloco de 64 bytes, montado so com as quatro instrucoes.
   Idioma habitual do AArch64: guardar abcd, sha256h, sha256h2 com a copia. */
static void sha256_block_via_insns(uint32_t* h, const uint8_t* block) {
    uint32_t w[4][4];
    uint32_t abcd[4] = {h[0], h[1], h[2], h[3]};
    uint32_t efgh[4] = {h[4], h[5], h[6], h[7]};
    int g, i, j;
    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i) {
            const int t = j * 4 + i;
            w[j][i] = ((uint32_t)block[t*4] << 24) | ((uint32_t)block[t*4+1] << 16) |
                      ((uint32_t)block[t*4+2] << 8) | (uint32_t)block[t*4+3];
        }
    for (g = 0; g < 16; ++g) {                 /* 16 grupos de 4 rondas */
        uint32_t wk[4], saved[4];
        for (i = 0; i < 4; ++i) wk[i] = w[g & 3][i] + K[g * 4 + i];
        memcpy(saved, abcd, 16);
        run3(0, abcd, efgh, wk);               /* SHA256H  abcd, efgh, wk */
        run3(1, efgh, saved, wk);              /* SHA256H2 efgh, saved, wk */
        if (g < 12) {                          /* estender W[4g+16 .. 4g+19] */
            const int a0 = g & 3, a1 = (g+1) & 3, a2 = (g+2) & 3, a3 = (g+3) & 3;
            run2(3, w[a0], w[a1]);             /* SHA256SU0 */
            run3(2, w[a0], w[a2], w[a3]);      /* SHA256SU1 */
        }
    }
    h[0] += abcd[0]; h[1] += abcd[1]; h[2] += abcd[2]; h[3] += abcd[3];
    h[4] += efgh[0]; h[5] += efgh[1]; h[6] += efgh[2]; h[7] += efgh[3];
}

int main(void) {
    int t, i;
    derive_K();
    /* Ancora barata nas constantes: o FIPS abre com 428a2f98 e fecha em c67178f2. */
    if (K[0] != 0x428a2f98u || K[63] != 0xc67178f2u) {
        printf("FAIL derive_K: K[0]=%08x K[63]=%08x\n", K[0], K[63]);
        return 1;
    }
    ++checks;

    /* SHA256SU0 isolado contra o modelo de referencia. */
    for (t = 0; t < 8; ++t) {
        uint32_t d[4], n[4], want[4], T[4];
        for (i = 0; i < 4; ++i) {
            d[i] = 0x01234567u * (uint32_t)(t + 1) + (uint32_t)i * 0x9e3779b9u;
            n[i] = 0xfedcba98u ^ ((uint32_t)(t * 4 + i) * 0x7f4a7c15u);
        }
        T[0] = d[1]; T[1] = d[2]; T[2] = d[3]; T[3] = n[0];
        for (i = 0; i < 4; ++i) want[i] = d[i] + ref_s0(T[i]);
        run2(3, d, n);
        if (memcmp(d, want, 16)) { printf("FAIL sha256su0 t=%d\n", t); return 1; }
        ++checks;
    }

    /* SHA256SU1 isolado contra o modelo de referencia. */
    for (t = 0; t < 8; ++t) {
        uint32_t d[4], n[4], m[4], want[4], T0[4], t1[2];
        for (i = 0; i < 4; ++i) {
            d[i] = 0x0a0b0c0du * (uint32_t)(t + 2) + (uint32_t)i;
            n[i] = 0x13579bdfu ^ ((uint32_t)(t + i) * 0x85ebca6bu);
            m[i] = 0x2468ace0u + (uint32_t)(t * 16 + i) * 0xc2b2ae35u;
        }
        T0[0] = n[1]; T0[1] = n[2]; T0[2] = n[3]; T0[3] = m[0];
        t1[0] = m[2]; t1[1] = m[3];
        for (i = 0; i < 2; ++i) want[i] = d[i] + T0[i] + ref_s1(t1[i]);
        for (i = 2; i < 4; ++i) want[i] = d[i] + T0[i] + ref_s1(want[i - 2]);
        run3(2, d, n, m);
        if (memcmp(d, want, 16)) { printf("FAIL sha256su1 t=%d\n", t); return 1; }
        ++checks;
    }

    /* A prova: SHA-256 completo de varias mensagens contra o OpenSSL. */
    for (t = 0; t < 7; ++t) {
        uint8_t msg[64], padded[64], ours[32], ossl[32];
        uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        unsigned len = 0;
        const size_t n = (size_t)t * 9;        /* 0, 9, ... 54 bytes: um bloco */
        for (i = 0; i < 64; ++i) msg[i] = (uint8_t)(t * 53 + i * 7 + 1);
        memset(padded, 0, sizeof padded);
        memcpy(padded, msg, n);
        padded[n] = 0x80;
        { const uint64_t bits = (uint64_t)n * 8;
          for (i = 0; i < 8; ++i) padded[56 + i] = (uint8_t)(bits >> (8 * (7 - i))); }
        sha256_block_via_insns(h, padded);
        for (i = 0; i < 8; ++i) {
            ours[i*4+0] = (uint8_t)(h[i] >> 24); ours[i*4+1] = (uint8_t)(h[i] >> 16);
            ours[i*4+2] = (uint8_t)(h[i] >> 8);  ours[i*4+3] = (uint8_t)h[i];
        }
        { EVP_MD_CTX* ctx = EVP_MD_CTX_new();
          EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
          EVP_DigestUpdate(ctx, msg, n);
          EVP_DigestFinal_ex(ctx, ossl, &len);
          EVP_MD_CTX_free(ctx); }
        if (memcmp(ours, ossl, 32)) {
            printf("FAIL sha256 t=%d (n=%zu)\n  nosso:", t, n);
            for (i = 0; i < 32; ++i) printf(" %02x", ours[i]);
            printf("\n  ossl :");
            for (i = 0; i < 32; ++i) printf(" %02x", ossl[i]);
            printf("\n");
            return 1;
        }
        ++checks;
    }
    if (sizeof functions / sizeof functions[0] != 4) return 1;
    printf("%d SHA-256 comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}

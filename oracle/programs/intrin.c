/* intrin.c - the oracle's instruction sampler: one C6000 intrinsic a line over values the compiler
 * cannot fold (they come through volatile), printed in hex, so the C674x's SIMD, saturating,
 * bit-field, multiply and floating-point instructions each run on TI's simulator and their results
 * are on record for vm6747 to match. */
#include <stdio.h>
#include <c6x.h>

volatile unsigned int va = 0x8001ff7fu, vb = 0x7ffe0103u, vc = 0x00ff80f0u, vd = 0xdeadbeefu;
volatile int vs = -1234567, vt = 89;
volatile float vf = 3.25f, vg = -0.1875f;
volatile double vx = 2.5, vy = -7.0625;

#define P(name, expr) printf("%-10s %08x\n", name, (unsigned int)(expr))

int main(void)
{
    unsigned int a = va, b = vb, c = vc, d = vd;
    int s = vs, t = vt;
    long long ll;
    P("add2", _add2(a, b));      P("sub2", _sub2(a, b));      P("add4", _add4(a, b));
    P("sub4", _sub4(a, b));      P("avg2", _avg2(a, b));      P("avgu4", _avgu4(a, b));
    P("abs2", _abs2(a));         P("bitc4", _bitc4(d));       P("bitr", _bitr(d));
    P("cmpeq2", _cmpeq2(a, a));  P("cmpgt2", _cmpgt2(a, b));  P("cmpgtu4", _cmpgtu4(a, b));
    P("deal", _deal(d));         P("shfl", _shfl(d));         P("dotp2", _dotp2(a, b));
    P("dotpn2", _dotpn2(a, b));  P("dotpu4", _dotpu4(a, b));  P("dotpsu4", _dotpsu4(a, b));
    P("lmbd", _lmbd(1, c));      P("norm", _norm(s));         P("max2", _max2(a, b));
    P("min2", _min2(a, b));      P("maxu4", _maxu4(a, b));    P("minu4", _minu4(a, b));
    P("mpyhi", _mpyhi(a, b));    P("mpyli", _mpyli(a, b));    P("mpyhir", _mpyhir(a, b));
    P("pack2", _pack2(a, b));    P("packh2", _packh2(a, b));  P("packh4", _packh4(a, b));
    P("packl4", _packl4(a, b));  P("packhl2", _packhl2(a, b)); P("packlh2", _packlh2(a, b));
    P("rotl", _rotl(d, t));      P("sadd", _sadd(s, 0x7fffffff)); P("sadd2", _sadd2(a, b));
    P("saddu4", _saddu4(a, b));  P("ssub", _ssub(s, 0x7fffffff)); P("ssub2", _ssub2(a, b));
    P("sshl", _sshl(s, 9));      P("spack2", _spack2(s, t));  P("spacku4", _spacku4(a, b));
    P("shlmb", _shlmb(a, b));    P("shrmb", _shrmb(a, b));    P("swap4", _swap4(d));
    P("subabs4", _subabs4(a, b)); P("unpkhu4", _unpkhu4(d));  P("unpklu4", _unpklu4(d));
    P("xpnd2", _xpnd2(5));       P("xpnd4", _xpnd4(9));
    P("ext", _ext(d, 4, 20));    P("extu", _extu(d, 4, 20));  P("set", _set(c, 3, 9));
    P("clr", _clr(d, 3, 9));     P("subc", _subc(0x40000, 7)); P("sat", _sat(((long long)s) << 8));
    P("mpy32", _mpy32(s, t));    P("shr2", _shr2(a, 3));      P("shru2", _shru2(a, 3));
    P("cmpy", _cmpy(a, b));      P("cmpyr", _cmpyr(a, b));
    ll = _mpy32ll(s, t);   P("mpy32ll.lo", ll); P("mpy32ll.hi", ll >> 32);
    P("rcpsp", _ftoi(_rcpsp(vf)));
    P("spint", _spint(vf * 1000.0f)); P("dpint", _dpint(vx * vy));
    P("mpysp", _ftoi(vf * vg));       P("addsp", _ftoi(vf + vg));
    ll = _dtoll(vx * vy);  P("mpydp.lo", ll); P("mpydp.hi", ll >> 32);
    ll = _dtoll(vx / vy);  P("divdp.lo", ll); P("divdp.hi", ll >> 32);
    P("csr", CSR & 0xffff0000u);
    return 0;
}

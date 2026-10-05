/* intrin2.c - the second half of the oracle's instruction sampler: one C6000 intrinsic a line over values the compiler
 * cannot fold (they come through volatile), printed in hex, so the C674x's SIMD, saturating,
 * bit-field, multiply and floating-point instructions each run on TI's simulator and their results
 * are on record for vm6747 to match. These are the C64x+ intrinsics
 * whose spelling in cl6x 7.4.4 is less certain, kept apart so that one unknown name costs one program. */
#include <stdio.h>
#include <c6x.h>

volatile unsigned int va = 0x8001ff7fu, vb = 0x7ffe0103u, vc = 0x00ff80f0u, vd = 0xdeadbeefu;
volatile int vs = -1234567, vt = 89;
volatile float vf = 3.25f, vg = -0.1875f;
volatile double vx = 2.5, vy = -7.0625;

#define P(name, expr) printf("%-10s %08x\n", name, (unsigned int)(expr))

int main(void)
{
    unsigned int a = va, b = vb;
    int s = vs, t = vt;
    long long ll;
    P("cmpyr1", _cmpyr1(a, b));
    P("xormpy", _xormpy(a, b));
    P("ssh", _sshvl(s, 5));
    P("sshvr", _sshvr(s, 5));
    P("gmpy4", _gmpy4(a, b));
    ll = _mpy2ll(a, b);    P("mpy2ll.lo", ll);  P("mpy2ll.hi", ll >> 32);
    ll = _ddotp4(a, b);    P("ddotp4.lo", ll);  P("ddotp4.hi", ll >> 32);
    ll = _dmv(a, b);       P("dmv.lo", ll);     P("dmv.hi", ll >> 32);
    ll = _addsub(s, t);    P("addsub.lo", ll);  P("addsub.hi", ll >> 32);
    P("rsqrsp", _ftoi(_rsqrsp(vf)));
    ll = _dtoll(_rcpdp(vy)); P("rcpdp.lo", ll); P("rcpdp.hi", ll >> 32);
    return 0;
}

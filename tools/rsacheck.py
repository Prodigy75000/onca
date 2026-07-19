# Verify GPU modexp: try real modulus + ASSEMBLED signature (from $F032xx).
# SPDX: GPL-3.0-or-later
mod_words = [0x0000002F,0xC50F79B7,0x961B10A2,0xEA46ABA1,0xF01DAFC5,0xC794C008,
             0xB981805E,0x5B93F503,0x0241FE75,0xB71CE8E7,0x2279A3D5,0xBE3045F9,
             0xEA35D98A,0x0A1540B4,0xB4E84EA6,0xDD17EE42,0x33100DF9]
# assembled signature operand at $F032A8..$F032E8 (17 words)
sig_words = [0x000000FF,0xFFFC3302,0x00F000CC,0x35FC330C,0xA1F10007,0x000700FC,
             0x230C21F0,0x00070007,0x00FC2300,0xA1F100C0,0x230021F0,0x00C02314,
             0xA1F100C0,0x231421F0,0x00C02300,0x700040F1,0x000001FC]
res_words = [0xA231A450,0x1DDF867A,0xF095A954,0xA2F7E908,0xCA949450,0x3954A2A9,
             0x1DE38F83,0x2F00B0AF,0x0FF1ED04,0xA5A7B5E2,0x2298486A,0xD67C327C,
             0x668A2F79,0x198DCD90,0x872A4F64,0xCAEBEC01,0x488A4CAD,0x72DCE1B8,
             0xA6BE8D34,0x03E805FB]

def w2i(ws, be=True):
    n = 0
    for w in (ws if be else ws[::-1]): n = (n << 32) | w
    return n

found = 0
for mdrop in (0, 1):                 # maybe first word 0x2F is a header
    for mbe in (True, False):
        n = w2i(mod_words[mdrop:], mbe)
        if n % 2 == 0: continue
        for sbe in (True, False):
            for sdrop in (0, 1):
                sig = w2i(sig_words[sdrop:], sbe) % n
                for e in list(range(2,40))+[257,0x10001]:
                    b = pow(sig, e, n)
                    for cut in (16,17,18):
                        for rbe in (True, False):
                            t = w2i(res_words[:cut], rbe)
                            if t >= n: continue
                            R = pow(2,32*cut,n)
                            for nm,v in (("plain",b),("*R",b*R%n),("*Ri",b*pow(R,-1,n)%n)):
                                if v == t:
                                    print("*** MATCH %s mdrop=%d mbe=%d sbe=%d sdrop=%d e=%d cut=%d rbe=%d"
                                          %(nm,mdrop,mbe,sbe,sdrop,e,cut,rbe)); found+=1
print("done, %d matches" % found)

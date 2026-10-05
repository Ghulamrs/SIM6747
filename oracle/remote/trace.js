// dss trace.js <ccxml> <program.out> <trace.txt> <cio.txt> [steps] [timeout ms]
// The oracle trace: TI's CCS 5.5 simulator steps a program one execute packet at a time from its
// load entry, and after every step writes the PC, all 64 general registers, the control registers it
// will give, and the CPU cycle count - the state vm6747 --trace-state writes, so the two diff line by line.
// The program's output through CIO goes to <cio.txt>. Nothing in the target is changed.
importPackage(Packages.com.ti.debug.engine.scripting);
importPackage(Packages.com.ti.ccstudio.scripting.environment);
importPackage(Packages.java.lang);
importPackage(Packages.java.io);
var env = ScriptingEnvironment.instance();
env.traceSetConsoleLevel(TraceLevel.WARNING);
env.setScriptTimeout(arguments.length > 5 ? parseInt(arguments[5]) : 3600000);
var steps = arguments.length > 4 ? parseInt(arguments[4]) : 20000;
var server = env.getServer("DebugServer.1");
server.setConfig(arguments[0]);
var session = server.openSession(".*");
session.target.connect();
var out = new PrintWriter(new FileWriter(arguments[2]));

// The clock events this simulator offers, by index: the header names them so the counts can be read.
var cpuEvent = -1;
for (var i = 0; i < 64; i++) {
    var n;
    try { n = session.clock.getEventName(i); } catch (e) { break; }
    if (n == null) break;
    out.println("# EVENT " + i + " " + n);
    if (cpuEvent < 0 && n.indexOf("CPU") >= 0) cpuEvent = i;
}
if (cpuEvent < 0) cpuEvent = 0;
session.clock.setCurrentEvent(cpuEvent);
session.clock.enable();
session.beginCIOLogging(arguments[3]);
session.memory.loadProgram(arguments[1]);
session.clock.reset();

var regs = [];
for (var r = 0; r < 32; r++) regs.push("A" + r);
for (var r = 0; r < 32; r++) regs.push("B" + r);
// Control registers: whichever of these this simulator names is traced; the rest are reported once.
var ctl = ["AMR", "CSR", "IER", "IFR", "IRP", "NRP", "ILC", "RILC", "SSR", "FADCR", "FAUCR", "FMCR",
           "GFPGFR", "GPLYA", "GPLYB", "TSR", "ITSR", "NTSR", "EFR", "IERR", "REP", "DNUM", "ECR"];
var have = [];
for (var c = 0; c < ctl.length; c++) {
    try { session.memory.readRegister(ctl[c]); have.push(ctl[c]); }
    catch (e) { out.println("# NOREG " + ctl[c]); }
}
out.println("# REGS PC " + regs.join(" ") + " " + have.join(" ") + " CYC");
function hex(v) { var s = Long.toHexString(v & 0xffffffff); while (s.length < 8) s = "0" + s; return s; }
function line(k) {
    var s = "S " + k + " " + hex(session.memory.readRegister("PC"));
    for (var j = 0; j < regs.length; j++) s += " " + hex(session.memory.readRegister(regs[j]));
    for (var j = 0; j < have.length; j++) s += " " + hex(session.memory.readRegister(have[j]));
    s += " " + session.clock.read();
    out.println(s);
}
var exitAt = -1;
try { exitAt = session.symbol.getAddress("C$$EXIT"); } catch (e) { }
out.println("# EXIT " + hex(exitAt));
line(0);
var t0 = System.currentTimeMillis();
var k = 1;
for (; k <= steps; k++) {
    session.target.asmStep.into();
    line(k);
    if (session.memory.readRegister("PC") == exitAt) break;
}
session.endCIOLogging();
out.println("# END steps=" + (k > steps ? steps : k) + " wall_ms=" + (System.currentTimeMillis() - t0));
out.close();
print("TRACE steps=" + (k > steps ? steps : k));
session.target.disconnect();
session.terminate();
server.stop();

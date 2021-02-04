contract C {
    uint public i;
    constructor(uint newI) {
        i = newI;
    }
}
contract D {
    C c;
    constructor(uint v) {
        c = new C(v);
    }
    function f() public returns (uint r) {
        return c.i();
    }
}
// ====
// compileViaYul: also
// ----
// constructor(): 2 ->
// gas irOptimized: 214602
// gas legacy: 243522
// gas legacyOptimized: 200912
// f() -> 2

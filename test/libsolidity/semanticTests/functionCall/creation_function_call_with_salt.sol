contract C {
    uint public i;
    constructor(uint newI) {
        i = newI;
    }
}
contract D {
    C c;
    constructor(uint v) {
        c = new C{salt: "abc"}(v);
    }
    function f() public returns (uint r) {
        return c.i();
    }
}
// ====
// EVMVersion: >=constantinople
// compileViaYul: also
// ----
// constructor(): 2 ->
// gas irOptimized: 214833
// gas legacy: 243894
// gas legacyOptimized: 201150
// f() -> 2

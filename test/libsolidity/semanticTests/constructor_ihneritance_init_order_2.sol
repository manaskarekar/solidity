contract A {
    uint x = 42;
    function f() public returns(uint256) {
        return x;
    }
}
contract B is A {
    uint public y = f();
}
// ====
// compileToEwasm: also
// compileViaYul: also
// ----
// constructor() ->
// gas irOptimized: 153851
// gas legacy: 131546
// gas legacyOptimized: 118014
// y() -> 42

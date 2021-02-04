contract Creator {
    uint256 public r;
    address public ch;

    constructor(address[3] memory s, uint256 x) {
        r = x;
        ch = s[2];
    }
}
// ====
// compileViaYul: also
// ----
// constructor(): 1, 2, 3, 4 ->
// gas irOptimized: 169280
// gas legacy: 172967
// gas legacyOptimized: 131795
// r() -> 4
// ch() -> 3

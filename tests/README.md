# Tests

## RecycleBin

The delete path has its own test because it is the one place in this
application that can destroy a person's data.

`src/RecycleBin.{h,cpp}` deliberately has no wxWidgets dependency, so it builds
and runs on its own:

```sh
g++ -std=c++17 -Wall -o tests/recyclebin_test.exe \
    tests/recyclebin_test.cpp src/RecycleBin.cpp -lole32 -lshell32 -luuid

./tests/recyclebin_test.exe
```

34 checks. They use real files in a scratch directory under `%TEMP%` and clean
up after themselves.

The assertions that matter are the ones comparing `SHQueryRecycleBinW`'s item
count before and after: they distinguish "the file is gone" from "the file is
recoverable". The implementation this replaced would have passed the first and
failed the second, which is precisely the bug.

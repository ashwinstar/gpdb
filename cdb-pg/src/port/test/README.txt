Directory with the following System Under Test (SUT):
 - memmem.c
 
Note that this is pretty poor infrastructure, which can't create
real/mocked files for running the tests. Therefore, each test file
is standalone and is executed without any dependencies.

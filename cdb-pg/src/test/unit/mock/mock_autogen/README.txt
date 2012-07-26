= Mock File Source Generation = 
 
Scripts to automatically generated cmockery mock files for GPDB.

How to add a files to the mock generation:
- Add the source filename to the mock_source file
- Call generate_mocks.py

In 99% of the cases, this was it.
 
= Details =
  
This topic is advanced and should not be relevant for someone developing tests.
The mock generation works in the following steps:

- The script walks over the cdb-pg directory tree and performs the following 
  steps for all C source files.
  
- It searches the associated header file for the source file. In certain special 
  cases, a source file has multiple associated header files. It is important 
  that all header files are handled by one and only one associated source file.
   
- The script generates a temporal source file consisting of the include files 
  of the original source file and the associated header files.
  
- In the next step, the C parser generates an Abstract Syntax Tree (AST) out of 
  the temporal source file. 
  
- We generate a mock function for all external, non-inline functions in the 
  associated header files. Usually this means that we have a check_expected call
  per parameter and an optional_assignment call for all non-const pointer 
  parameters. 
  We also generate a mock() call for void-functions and a returning mock() call 
  for non-void functions.
  
- The reality is messy. For certain functions, we need special handling. For 
  certain return and parameter types, we need special code generation. We deal 
  with this by having a default code generation implementation and a number of 
  special cases handled differently. These special cases are activated by 
  function name or by parameter type.
  
- We also generate a global variable for all external non-static variables.
 
= Mock Function File Troubleshooting = 

The mock generation is pretty stable, but there are a few common issues that 
come up every once 
in a while:

- Issue: There is no mock function for a given function in the mock directory

Probably it was previously not necessary to have the mock function file for the 
source file. We only store the mock functions for actually used source files. 
Please add the source filename to the mock_source file and call 
generate_mocks.py. The mock function file for the source file should be there
 now.

- Issue: The compilation of a mock source file fails because a mock function is 
         no longer in sync with the header file.
         
Call generate_mocks.py to regenerate the mock files.

- Issue: The generation of mock files fails because of a parser error

There are typically two reasons for this: The first one is that a parameter name 
in a header file is equal to a type. This is really difficult to parse correctly 
and the current C parser fails at this task. As it is a really bad style anyway, 
it might be appropriate to change the parameter name to something that is not a 
valid type. Another reason can be that the header or the source file includes 
header files that are not available on the given system. You should add 
these header files to the ignore list in generate_mocks.py.

- Issue: The compilation of a mock function file fails because of a 
check_expected call.
 
Probably the mock function has a struct parameter that is passed by-value. These 
structs need special handling. Please add the special handling to 
generate_mocks.py. You can see the function generate_check_Interval as an 
example.

# CMake generated Testfile for 
# Source directory: /workspace
# Build directory: /workspace/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(session "/workspace/build/test_session")
set_tests_properties(session PROPERTIES  _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;20;add_test;/workspace/CMakeLists.txt;0;")
add_test(tools "/workspace/build/test_tools")
set_tests_properties(tools PROPERTIES  _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;23;add_test;/workspace/CMakeLists.txt;0;")
add_test(loop_offline "/workspace/build/test_loop_offline")
set_tests_properties(loop_offline PROPERTIES  _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;26;add_test;/workspace/CMakeLists.txt;0;")

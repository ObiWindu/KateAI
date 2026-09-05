# CMake generated Testfile for 
# Source directory: /home/macewindu/ai-dev/KateAI
# Build directory: /home/macewindu/ai-dev/KateAI/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[appstreamtest]=] "/usr/bin/cmake" "-DAPPSTREAMCLI=/usr/bin/appstreamcli" "-DINSTALL_FILES=/home/macewindu/ai-dev/KateAI/build/install_manifest.txt" "-P" "/usr/share/ECM/kde-modules/appstreamtest.cmake")
set_tests_properties([=[appstreamtest]=] PROPERTIES  _BACKTRACE_TRIPLES "/usr/share/ECM/kde-modules/KDECMakeSettings.cmake;177;add_test;/usr/share/ECM/kde-modules/KDECMakeSettings.cmake;195;appstreamtest;/usr/share/ECM/kde-modules/KDECMakeSettings.cmake;0;;/home/macewindu/ai-dev/KateAI/CMakeLists.txt;47;include;/home/macewindu/ai-dev/KateAI/CMakeLists.txt;0;")
subdirs("src")
subdirs("tests")

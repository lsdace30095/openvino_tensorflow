 # Copyright (C) 2021 Intel Corporation

 # SPDX-License-Identifier: Apache-2.0

set(LIB_NAME openvino_tensorflow)

include_directories(${TensorFlow_INCLUDE_DIR})
include_directories(${TensorFlow_INCLUDE_DIR}/external/nsync/public)
include_directories(${NGRAPH_INSTALL_DIR}/include)
include_directories(${ABSL_INCLUDE_DIR})

# For some reason the following is needed for ABI compatibility with TF.
# (There must be some dependency on it in the struct/class definitions.)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNDEBUG")

message(STATUS "Compile Flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "Shared Link Flags: ${CMAKE_SHARED_LINKER_FLAGS}")

#-----------------------------------------------------------------------------------------------
# Compiler-specific logic...
#-----------------------------------------------------------------------------------------------
set(SRC
   api.cc
   backend.cc
   backend_manager.cc
   executable.cc
   ie_tensor.cc
   kernels/encapsulate_op.cc
   assign_clusters.cc
   ovtf_builder.cc
   cluster_manager.cc
   layout_conversions.cc
   deassign_clusters.cc
   encapsulate_clusters.cc
   mark_for_clustering.cc
   rewrite_pass.cc
   ovtf_utils.cc
   ops/encapsulate_op.cc
   pass/transpose_sinking.cc
   tf_graphcycles.cc
   tf_deadness_analysis.cc
   version.cc
   ie_backend_engine.cc
   ie_basic_engine.cc
   ie_vadm_engine.cc
)

message(STATUS "OPENVINO_TF_USE_GRAPPLER_OPTIMIZER: ${OPENVINO_TF_USE_GRAPPLER_OPTIMIZER}")
if(OPENVINO_TF_USE_GRAPPLER_OPTIMIZER)
    list(REMOVE_ITEM SRC rewrite_pass.cc)
    list(APPEND SRC grappler/ovtf_optimizer.cc)
    list(APPEND SRC grappler/add_identityn.cc)
    add_definitions(-DOPENVINO_TF_USE_GRAPPLER_OPTIMIZER)
endif()

if(OPENVINO_BUILD_VERSION)
    add_compile_definitions(OPENVINO_BUILD_VERSION="${OPENVINO_BUILD_VERSION}")
endif()

add_library(${LIB_NAME} SHARED ${SRC})

if(WIN32)
  get_target_property(ngraph_lib_value ngraph_lib IMPORTED_LOCATION)
  target_link_libraries(
            ${LIB_NAME}
            ovtf_logger
            ${TensorFlow_FRAMEWORK_LIBRARY}
            ${ngraph_lib_value}
            absl_algorithm
            absl_container
            absl_strings
            ${InferenceEngine_LIBRARIES}
            ocm
    )
else()
  target_link_libraries(
          ${LIB_NAME}
          ovtf_logger
          ${TensorFlow_FRAMEWORK_LIBRARY}
          ngraph_lib
          absl_algorithm
          absl_container
          absl_strings
          ${InferenceEngine_LIBRARIES}
          ocm
      )
endif()

target_compile_definitions(
    ${LIB_NAME} PRIVATE
    _GLIBCXX_USE_CXX11_ABI=${TensorFlow_CXX_ABI}
)

target_include_directories(${LIB_NAME} PUBLIC "${NGRAPH_INSTALL_DIR}/include")

string(REPLACE "." ";" TF_VERSION_LIST ${TensorFlow_VERSION})
list(GET TF_VERSION_LIST 0 TF_MAJOR_VERSION)
add_compile_definitions(TF_VERSION=${TF_MAJOR_VERSION})

#------------------------------------------------------------------------------
#installation
#------------------------------------------------------------------------------
if (DEFINED OPENVINO_TF_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX ${OPENVINO_TF_INSTALL_PREFIX})
else()
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/../install/")
endif()

if(OS_VERSION STREQUAL "\"centos\"")
	set(OVTF_INSTALL_LIB_DIR ${CMAKE_INSTALL_PREFIX}/lib64)
else()
	set(OVTF_INSTALL_LIB_DIR ${CMAKE_INSTALL_PREFIX}/lib)
endif()

message(STATUS "OVTF_INSTALL_LIB_DIR: ${OVTF_INSTALL_LIB_DIR}")

# First install the libopenvino_tensorflow.so and headers
install(TARGETS ${LIB_NAME} DESTINATION "${OVTF_INSTALL_LIB_DIR}")

# Next install all the other prerequisites
if(OS_VERSION STREQUAL "\"centos\"")
	install(DIRECTORY ${NGRAPH_INSTALL_DIR}/lib64/ DESTINATION "${OVTF_INSTALL_LIB_DIR}" )
else()
  if(WIN32)
    file(GLOB NGRAPH_LIB_FILES "${NGRAPH_INSTALL_DIR}/lib/*")
    foreach(LIB_FILE ${NGRAPH_LIB_FILES})
      get_filename_component(lib_file_real_path ${LIB_FILE} ABSOLUTE)
      get_filename_component(lib_file_name ${LIB_FILE} NAME)
      if(NOT(${lib_file_name} MATCHES "onnx.*"))
        install(FILES ${lib_file_real_path} DESTINATION "${OVTF_INSTALL_LIB_DIR}")
      endif()
    endforeach()
  else()
    install(DIRECTORY ${NGRAPH_INSTALL_DIR}/lib/ DESTINATION "${OVTF_INSTALL_LIB_DIR}" )
  endif()
endif()

# Install OpenVINO libraries
if (APPLE)
  set(OCM_LIB libocm.dylib)
elseif(WIN32)
  set(OCM_LIB ocm.dll)
else()
  set(OCM_LIB libocm.so)
endif()

if(WIN32)
  set(IE_LIBS ${OPENVINO_ARTIFACTS_DIR}/deployment_tools/inference_engine/bin/intel64/Release/)
  set(TBB_LIBS ${OPENVINO_ARTIFACTS_DIR}/deployment_tools/inference_engine/external/tbb/bin/)
  install(FILES ${CMAKE_INSTALL_PREFIX}/../ocm/OCM/Release/${OCM_LIB} DESTINATION "${OVTF_INSTALL_LIB_DIR}")
else()
  set(IE_LIBS ${OPENVINO_ARTIFACTS_DIR}/deployment_tools/inference_engine/lib/intel64/)
  set(TBB_LIBS ${OPENVINO_ARTIFACTS_DIR}/deployment_tools/inference_engine/external/tbb/lib/)
  install(FILES ${CMAKE_INSTALL_PREFIX}/../ocm/OCM/${OCM_LIB} DESTINATION "${OVTF_INSTALL_LIB_DIR}")
endif()
if(NOT DISABLE_PACKAGING_OPENVINO_LIBS)
 install(DIRECTORY ${IE_LIBS} DESTINATION "${OVTF_INSTALL_LIB_DIR}" )
 install(DIRECTORY ${TBB_LIBS} DESTINATION "${OVTF_INSTALL_LIB_DIR}" )
    if(USE_OPENVINO_FROM_LOCATION)
      if(WIN32)
        file(GLOB NGRAPH_LIB_FILES "${IE_LIBS}/*" "${TBB_LIBS}/*")
        foreach(LIB_FILE ${NGRAPH_LIB_FILES})
          get_filename_component(lib_file_real_path ${LIB_FILE} ABSOLUTE)
          get_filename_component(lib_file_name ${LIB_FILE} NAME)
          if(${lib_file_name} MATCHES ".dll*")
            install(CODE "execute_process(COMMAND patchelf --set-rpath $ORIGIN ${lib_file_real_path} COMMAND_ECHO STDOUT)")
          endif()
        endforeach()
      else()
        file(GLOB NGRAPH_LIB_FILES "${IE_LIBS}/lib*" "${TBB_LIBS}/lib*")
        foreach(LIB_FILE ${NGRAPH_LIB_FILES})
            get_filename_component(lib_file_real_path ${LIB_FILE} ABSOLUTE)
            get_filename_component(lib_file_name ${LIB_FILE} NAME)
            if(${lib_file_name} MATCHES ".so*")
                install(CODE "
                execute_process(COMMAND patchelf --set-rpath $ORIGIN ${lib_file_real_path} COMMAND_ECHO STDOUT)
                ")
            endif()
          endforeach()
        endif()
    endif()
endif()

# Now create the Python pip package. The following variables are passed to the
# CreatePipWhl.cmake
# CMake "install" target "CODE" will propagate these variables to the
# target "SCRIPT" so that the "SCRIPT" (in ths case CreatePipWhl.cmake)
# So any variable that we need to propagate needs to be added here
install(CODE "set(OS_VERSION ${OS_VERSION})")
install(CODE "set(NGRAPH_INSTALL_DIR \"${NGRAPH_INSTALL_DIR}\")")
install(CODE "set(OVTF_SRC_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/../\")")
install(CODE "set(OVTF_INSTALL_DIR \"${CMAKE_INSTALL_PREFIX}\")")
install(CODE "set(TensorFlow_GIT_VERSION \"${TensorFlow_GIT_VERSION}\")")
install(CODE "set(TensorFlow_VERSION \"${TensorFlow_VERSION}\")")
install(CODE "set(CMAKE_BUILD_TYPE \"${CMAKE_BUILD_TYPE}\")")
if(PYTHON_EXECUTABLE)
    install(CODE "set(PYTHON_EXECUTABLE \"${PYTHON_EXECUTABLE}\")")
endif()
install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../python/CreatePipWhl.cmake")

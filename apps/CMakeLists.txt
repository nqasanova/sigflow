foreach(app enb ue)
    add_executable(sigflow-${app} ${app}_sim.cpp)
    target_link_libraries(sigflow-${app} PRIVATE ran_messages)
    sigflow_set_warnings(sigflow-${app})
endforeach()

install(TARGETS sigflow-enb sigflow-ue RUNTIME DESTINATION bin)

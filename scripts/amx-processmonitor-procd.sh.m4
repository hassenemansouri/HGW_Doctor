#!/bin/sh /etc/rc.common

. /usr/lib/amx/scripts/amx_procd_init_functions.sh


START=START_ORDER
STOP=STOP_ORDER
USE_PROCD=1

name="hgw_doctor"
PROG="/usr/bin/hgw_doctor"
datamodel_root="HGW_Doctor"
PROG_OPTIONS=""

#Guideline- uncomment this function and place the instructions
#for functionality to execute before starting this service
#End

#preservice_hook() {
#    logger -s "pre-service hook ${name}"
#}

#Guideline- uncomment this function and place the instructions
#for functionality to execute after stopping this service
#End

#postservice_hook() {
#    logger -s "post-service hook ${name}"
#}

start_service() {
    #preservice_hook
    register_service ${name} ${PROG} ${PROG_OPTIONS}
}

stop_service() {
    deregister_service ${name}
    #postservice_hook
}


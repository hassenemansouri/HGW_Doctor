#!/bin/sh

. /usr/lib/amx/scripts/amx_init_functions.sh

name="hgw_doctor"
datamodel_root="HGW_Doctor"

case $1 in
    boot)
        process_boot ${name} -D
        ;;
    start)
        process_start ${name} -D
        ;;
    stop)
        process_stop ${name}
        ;;
    shutdown)
        process_shutdown ${name}
        ;;
    restart)
        $0 stop
        $0 start
        ;;
    debuginfo)
        process_debug_info ${datamodel_root}
        ;;
    *)
        echo "Usage : $0 [start|boot|stop|shutdown|debuginfo|restart]"
        ;;
esac

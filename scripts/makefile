all:
	m4 -D START_ORDER=$(CONFIG_HGW_DOCTOR_START_ORDER) -D STOP_ORDER=$(CONFIG_HGW_DOCTOR_STOP_ORDER) hgw_doctor-procd.sh.m4 > hgw_doctor-procd.sh

clean:
	rm -rf hgw_doctor-procd.sh

#
# All these targets must be run regardless of the presence of a file
# named after them.
#
.PHONY: all clean


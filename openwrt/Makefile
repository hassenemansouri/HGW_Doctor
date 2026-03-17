include $(TOPDIR)/rules.mk

PKG_NAME    := hgw-doctor
PKG_VERSION := 0.1.0
PKG_RELEASE := 1

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

# -----------------------------------------------------------------------
# Package metadata
# -----------------------------------------------------------------------
define Package/hgw-doctor
  SECTION  := utils
  CATEGORY := Utilities
  TITLE    := HGW-Doctor - Home Gateway Health Monitor
  DEPENDS  := +libamxd +libamxo +libamxc +libamxp +libamxs +libcurl +libpthread
endef

define Package/hgw-doctor/description
  Lightweight autonomous monitoring and recovery daemon for residential gateways.
  Monitors CPU, memory and process health via /proc, triggers recovery actions,
  collects diagnostics and uploads them. Exposes a TR-181 data model via Ambiorix.
endef

# -----------------------------------------------------------------------
# Prepare: copy source tree into the build directory
# -----------------------------------------------------------------------
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
	rm -rf $(PKG_BUILD_DIR)/build $(PKG_BUILD_DIR)/openwrt
endef

# -----------------------------------------------------------------------
# Compile: call the existing Makefile with the OpenWrt cross-toolchain
# -----------------------------------------------------------------------
define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		STAGING_DIR="$(STAGING_DIR)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

# -----------------------------------------------------------------------
# Install: place files into the package image
# -----------------------------------------------------------------------
define Package/hgw-doctor/install
	# Daemon binary
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/bin/hgw-doctor        $(1)/usr/sbin/

	# Ambiorix callback shared library.
	# /usr/lib/hgw_doctor.so        — dynamic linker finds it here at startup
	# /etc/amx/hgw_doctor/ symlink  — amxo parser searches the ODL directory
	$(INSTALL_DIR) $(1)/usr/lib
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/lib/hgw_doctor.so     $(1)/usr/lib/
	$(INSTALL_DIR) $(1)/etc/amx/hgw_doctor
	ln -sf /usr/lib/hgw_doctor.so                               $(1)/etc/amx/hgw_doctor/hgw_doctor.so

	# Config file
	$(INSTALL_DIR) $(1)/etc/hgw_doctor
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/conf/hgw_doctor.conf       $(1)/etc/hgw_doctor/

	# ODL files (TR-181 data model)
	$(INSTALL_DIR) $(1)/etc/amx/hgw_doctor
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/odl/hgw_doctor.odl              $(1)/etc/amx/hgw_doctor/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/odl/hgw_doctor_defaults.odl     $(1)/etc/amx/hgw_doctor/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/odl/hgw_doctor_caps.odl         $(1)/etc/amx/hgw_doctor/

	# Recovery action scripts
	$(INSTALL_DIR) $(1)/usr/lib/hgw_doctor/actions
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/actions/restart_process.sh  $(1)/usr/lib/hgw_doctor/actions/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/actions/clear_cache.sh      $(1)/usr/lib/hgw_doctor/actions/

	# procd init script
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/hgw-doctor.init                      $(1)/etc/init.d/hgw-doctor
endef

$(eval $(call BuildPackage,hgw-doctor))

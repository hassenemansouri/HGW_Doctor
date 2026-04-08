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
# Prepare: copy the full source tree into the build directory
# $(CURDIR) = .../package/hgw-doctor (the openwrt/ dir in your repo)
# So the project root is one level up: $(CURDIR)/..
# -----------------------------------------------------------------------
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(TOPDIR)/feeds/hgw_doctor/src         $(PKG_BUILD_DIR)/src
	$(CP) $(TOPDIR)/feeds/hgw_doctor/include     $(PKG_BUILD_DIR)/include
	$(CP) $(TOPDIR)/feeds/hgw_doctor/odl         $(PKG_BUILD_DIR)/odl
	$(CP) $(TOPDIR)/feeds/hgw_doctor/conf        $(PKG_BUILD_DIR)/conf
	$(CP) $(TOPDIR)/feeds/hgw_doctor/actions     $(PKG_BUILD_DIR)/actions
	$(CP) $(TOPDIR)/feeds/hgw_doctor/Makefile    $(PKG_BUILD_DIR)/Makefile
	$(CP) $(TOPDIR)/feeds/hgw_doctor/makefile.inc $(PKG_BUILD_DIR)/makefile.inc
	rm -rf $(PKG_BUILD_DIR)/build
endef

# -----------------------------------------------------------------------
# Compile: call the project Makefile with the OpenWrt cross-toolchain
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

	# Ambiorix callback shared library
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

	# TR-181 device proxy mapping
	$(INSTALL_DIR) $(1)/etc/amx/tr181-device/extensions
	$(INSTALL_DATA) $(TOPDIR)/feeds/hgw_doctor/hgw-doctor/files/01_device-hgw_doctor_mapping.odl \
		$(1)/etc/amx/tr181-device/extensions/

	# procd init script
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(TOPDIR)/feeds/hgw_doctor/openwrt/files/hgw-doctor.init $(1)/etc/init.d/hgw-doctor
endef

$(eval $(call BuildPackage,hgw-doctor))

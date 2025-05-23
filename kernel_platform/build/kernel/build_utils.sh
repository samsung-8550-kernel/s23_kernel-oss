#!/bin/bash

# Copyright (C) 2021 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# TODO (b/231473697): rel_path and rel_path2 should use realpath --relative-to
# rel_path <to> <from>
# Generate relative directory path to reach directory <to> from <from>
function rel_path() {
  local to=$1
  local from=$2
  local path=
  local stem=
  local prevstem=
  [ -n "$to" ] || return 1
  [ -n "$from" ] || return 1
  to=$(readlink -e "$to")
  from=$(readlink -e "$from")
  [ -n "$to" ] || return 1
  [ -n "$from" ] || return 1
  stem=${from}/
  while [ "${to#$stem}" == "${to}" -a "${stem}" != "${prevstem}" ]; do
    prevstem=$stem
    stem=$(readlink -e "${stem}/..")
    [ "${stem%/}" == "${stem}" ] && stem=${stem}/
    path=${path}../
  done
  echo ${path}${to#$stem}
}

# TODO (b/231473697): rel_path and rel_path2 should use realpath --relative-to
# rel_path2 <to> <from>
# Generate relative directory path to reach directory <to> from <from>
# This is slower than rel_path, but returns a simpler path when <from>
# is directly under <to>.
function rel_path2() {
  local to=$1
  local from=$2
  python3 -c 'import os,sys;print(os.path.relpath(*(sys.argv[1:])))' "$to" "$from"
}

# $1 directory of kernel modules ($1/lib/modules/x.y)
# $2 flags to pass to depmod
# $3 kernel version
# $4 Optional: File with list of modules to run depmod on.
#              If left empty, depmod will run on all modules
#              under $1/lib/modules/x.y
function run_depmod() {
  (
    local ramdisk_dir=$1
    local depmod_stdout
    local depmod_stderr=$(mktemp)
    local version=$3
    local modules_list_file=$4
    local modules_list=""

    if [[ -n "${modules_list_file}" ]]; then
      while read -r line; do
        # depmod expects absolute paths for module files
        modules_list+="${ramdisk_dir}/lib/modules/${version}/${line} "
      done <${modules_list_file}
    fi

    cd ${ramdisk_dir}
    if ! depmod_stdout="$(depmod $2 -F ${DIST_DIR}/System.map -b . ${version} ${modules_list} \
        2>${depmod_stderr})"; then
      echo "$depmod_stdout"
      cat ${depmod_stderr} >&2
      rm -f ${depmod_stderr}
      exit 1
    fi
    [ -n "$depmod_stdout" ] && echo "$depmod_stdout"
    cat ${depmod_stderr} >&2
    if { grep -q "needs unknown symbol" ${depmod_stderr}; }; then
      echo "ERROR: kernel module(s) need unknown symbol(s)" >&2
      rm -f ${depmod_stderr}
      exit 1
    fi
    rm -f ${depmod_stderr}
  )
}

# $1 MODULES_LIST <File that contains the list of modules that need to be
#                   loaded during first-stage init.>
# $2 ADDITIONAL_MODULES_LIST <File that contains the list of additional modules
#                           that need to be loaded from the initramfs.>
# $3 MODULES_ORDER <File that contains the list of all modules in the order in which
#                   they appear in Makefiles.>
# $4 MODULES_OUTPUT_FILENAME <The name of the output modules.order file>
function create_additional_modules_order() {
  local modules_list_file=$1
  local additional_modules_file=$2
  local modules_order_file=$3
  local modules_out_filename=$4
  local tmp_all_modules_list_file

  if [[ -f "${ROOT_DIR}/${additional_modules_file}" ]]; then
    additional_modules_file="${ROOT_DIR}/${additional_modules_file}"
  elif [[ "${additional_modules_file}" != /* ]]; then
    echo "modules recovery list must be an absolute path or relative to ${ROOT_DIR}: ${additional_modules_file}" >&2
    exit 1
  elif [[ ! -f "${additional_modules_file}" ]]; then
    echo "Failed to find modules list: ${additional_modules_file}" >&2
    exit 1
  fi

  tmp_all_modules_list_file=$(mktemp)

  cp ${modules_list_file} ${tmp_all_modules_list_file}
  # Append to the first stage init modules
  grep -v "^\#" ${additional_modules_file} >> ${tmp_all_modules_list_file}
  grep -w -f ${tmp_all_modules_list_file} ${modules_order_file} > ${modules_out_filename}

  rm ${tmp_all_modules_list_file}
}

# $1 MODULES_LIST, <File contains the list of modules that should go in the ramdisk>
# $2 MODULES_STAGING_DIR    <The directory to look for all the compiled modules>
# $3 IMAGE_STAGING_DIR  <The destination directory in which MODULES_LIST is
#                        expected, and it's corresponding modules.* files>
# $4 MODULES_BLOCKLIST, <File contains the list of modules to prevent from loading>
# $5 MODULES_RECOVERY_LIST <File contains the list of modules that should go in
#                           the ramdisk but should only be loaded when booting
#                           into recovery.
#
#                           This parameter is optional, and if not used, should
#                           be passed as an empty string to ensure that the depmod
#                           flags are assigned correctly.>
# $6 MODULES_CHARGER_LIST <File contains the list of modules that should go in
#                          the ramdisk but should only be loaded when booting
#                          into charger mode.
#
#                          This parameter is optional, and if not used, should
#                          be passed as an empty string to ensure that the
#                          depmod flags are assigned correctly.>
# $7 flags to pass to depmod
function create_modules_staging() {
  local modules_list_file=$1
  local src_dir=$(echo $2/lib/modules/*)
  local version=$(basename "${src_dir}")
  local dest_dir=$3/lib/modules/${version}
  local dest_stage=$3
  local modules_blocklist_file=$4
  local modules_recoverylist_file=$5
  local modules_chargerlist_file=$6
  local depmod_flags=$7
  local list_order=$8

  rm -rf ${dest_dir}
  mkdir -p ${dest_dir}/kernel
  find ${src_dir}/kernel/ -maxdepth 1 -mindepth 1 \
    -exec cp -r {} ${dest_dir}/kernel/ \;
  # The other modules.* files will be generated by depmod
  cp ${src_dir}/modules.order ${dest_dir}/modules.order
  cp ${src_dir}/modules.builtin ${dest_dir}/modules.builtin

  if [[ -n "${EXT_MODULES}" ]] || [[ -n "${EXT_MODULES_MAKEFILE}" ]]; then

    # SS Kbuild Error Fix
    if [[ -n "${KBUILD_EXT_MODULES}" ]]; then
      cp -r ${src_dir}/* ${dest_dir}/
    else
      mkdir -p ${dest_dir}/extra/
      cp -r ${src_dir}/extra/* ${dest_dir}/extra/
    fi
    
    # Check if we have modules.order files for external modules. This is
    # supported in android-mainline since 5.16 and androidX-5.15
    FIND_OUT=$(find ${dest_dir}/extra -name modules.order.* -print -quit)
    if [[ -n "${EXT_MODULES}" ]] && [[ "${FIND_OUT}" =~ modules.order ]]; then
      # If EXT_MODULES is defined and we have modules.order.* files for
      # external modules, then we should follow this module load order:
      #   1) Load modules in order defined by EXT_MODULES.
      #   2) Within a given external module, load in order defined by
      #      modules.order.
      for EXT_MOD in ${EXT_MODULES}; do
        # Since we set INSTALL_MOD_DIR=extra/${EXTMOD}, we can directly use the
        # modules.order.* file at that path instead of tring to figure out the
        # full name of the modules.order file. This is complicated because we
        # set M=... to a relative path which can't easily be calculated here
        # when using kleaf due to sandboxing.
        modules_order_file=$(ls ${dest_dir}/extra/${EXT_MOD}/modules.order.*)
        if [[ -f "${modules_order_file}" ]]; then
          cat ${modules_order_file} >> ${dest_dir}/modules.order
        else
          # We need to fail here; otherwise, you risk the module(s) not getting
          # included in modules.load.
          echo "Failed to find ${modules_order_file}" >&2
          exit 1
        fi
      done
    else
      # TODO: can we retain modules.order when using EXT_MODULES_MAKEFILE? For
      # now leave this alone since EXT_MODULES_MAKEFILE isn't support in v5.13+.
      (cd ${dest_dir}/ && \
        find extra -type f -name "*.ko" | sort >> modules.order)
    fi
  fi

  if [ "${DO_NOT_STRIP_MODULES}" = "1" ]; then
    # strip debug symbols off initramfs modules
    find ${dest_dir} -type f -name "*.ko" \
      -exec ${OBJCOPY:-${CROSS_COMPILE}objcopy} --strip-debug {} \;
  fi

  if [ -n "${modules_list_file}" ]; then
    # Need to make sure we can find modules_list_file from the staging dir
    if [[ -f "${ROOT_DIR}/${modules_list_file}" ]]; then
      modules_list_file="${ROOT_DIR}/${modules_list_file}"
    elif [[ "${modules_list_file}" != /* ]]; then
      echo "modules list must be an absolute path or relative to ${ROOT_DIR}: ${modules_list_file}"
      exit 1
    elif [[ ! -f "${modules_list_file}" ]]; then
      echo "Failed to find modules list: ${modules_list_file}"
      exit 1
    fi

    local modules_list_filter=$(mktemp)
    local old_modules_list=$(mktemp)

    # Remove all lines starting with "#" (comments)
    # Exclamation point makes interpreter ignore the exit code under set -e
    ! grep -v "^\#" ${modules_list_file} > ${modules_list_filter}

    # grep the modules.order for any KOs in the modules list
    cp ${dest_dir}/modules.order ${old_modules_list}
    if [ "${list_order}" = "1" ]; then
      sed -i 's/.*\///g' ${old_modules_list}
      ! grep -x -f ${old_modules_list} ${modules_list_filter} > ${dest_dir}/modules.order
    else
      ! grep -w -f ${modules_list_filter} ${old_modules_list} > ${dest_dir}/modules.order
    fi
    if [[ -n "${modules_recoverylist_file}" ]]; then
      create_additional_modules_order "${modules_list_filter}" "${modules_recoverylist_file}" \
        "${old_modules_list}" "${dest_dir}/modules.order.recovery"
    fi
    if [[ -n "${modules_chargerlist_file}" ]]; then
      create_additional_modules_order "${modules_list_filter}" "${modules_chargerlist_file}" \
        "${old_modules_list}" "${dest_dir}/modules.order.charger"
    fi
    rm -f ${modules_list_filter} ${old_modules_list}
  fi

  if [ -n "${modules_blocklist_file}" ]; then
    # Need to make sure we can find modules_blocklist_file from the staging dir
    if [[ -f "${ROOT_DIR}/${modules_blocklist_file}" ]]; then
      modules_blocklist_file="${ROOT_DIR}/${modules_blocklist_file}"
    elif [[ "${modules_blocklist_file}" != /* ]]; then
      echo "modules blocklist must be an absolute path or relative to ${ROOT_DIR}: ${modules_blocklist_file}"
      exit 1
    elif [[ ! -f "${modules_blocklist_file}" ]]; then
      echo "Failed to find modules blocklist: ${modules_blocklist_file}"
      exit 1
    fi

    if [ "${SEC_BUILD_OPTION_KUNIT}" == "true" ]; then
      local modules_blocklist_file_kunit="${ROOT_DIR}/msm-kernel/modules.vendor_blocklist.kunit"
      local orig_perm_writable="yes"

      if [ -e "${modules_blocklist_file_kunit}" ]; then
        echo "Insert KUnit module blocklist into: ${modules_blocklist_file}"
        if [[ ! -w "${modules_blocklist_file}" ]]; then
          orig_perm_writable="no"
          chmod +w "${modules_blocklist_file}"
        fi
        echo >> "${modules_blocklist_file}"
        cat "${modules_blocklist_file_kunit}" >> "${modules_blocklist_file}"
        cat "${modules_blocklist_file}"
        if [[ "${orig_perm_writable}" == "no" ]]; then
          chmod -w "${modules_blocklist_file}"
        fi
      fi
    fi

    cp ${modules_blocklist_file} ${dest_dir}/modules.blocklist
  fi

  if [ -n "${TRIM_UNUSED_MODULES}" ]; then
    echo "========================================================"
    echo " Trimming unused modules"
    local used_blocklist_modules=$(mktemp)
    if [ -f ${dest_dir}/modules.blocklist ]; then
      # TODO: the modules blocklist could contain module aliases instead of the filename
      sed -n -E -e 's/blocklist (.+)/\1/p' ${dest_dir}/modules.blocklist > $used_blocklist_modules
    fi

    # Trim modules from tree that aren't mentioned in modules.order
    (
      cd ${dest_dir}
      if [ "${list_order}" = "1" ]; then
        find . -type f -name '*.ko' -printf "%f\n" > modules.name.full
        cat modules.order $used_blocklist_modules > modules.name.need
        grep -v -x -f modules.name.need modules.name.full > modules.name.remove
        # This is done to insert escaped directory boundary '\/' in front of every line (eg \/a.ko)
        # It is for matching entire module's name with 'grep -f' instead of 'grep -w -f'
        # so that b-a.ko is not same with a.ko
        sed -i 's/^/\\\/&/' modules.name.remove
        find * -type f -name "*.ko" | grep -f modules.name.remove - | xargs -r rm
        rm -rf modules.name.full modules.name.need modules.name.remove
      else
        local grep_flags="-v -w -f modules.order -f ${used_blocklist_modules} "
        [[ -f modules.order.recovery ]] && grep_flags+="-f modules.order.recovery "
        [[ -f modules.order.charger ]] && grep_flags+="-f modules.order.charger "
        find * -type f -name "*.ko" | (grep ${grep_flags} - || true) | xargs -r rm
      fi
    )
    rm $used_blocklist_modules
  fi

  # Run depmod to ensure that dependencies between all modules loaded during
  # first stage init when booting into any other mode besides normal boot can be
  # satisfied.
  if [[ -f ${dest_dir}/modules.order.recovery ]]; then
    run_depmod ${dest_stage} "${depmod_flags}" "${version}" "${dest_dir}/modules.order.recovery"
    cp ${dest_dir}/modules.order.recovery ${dest_dir}/modules.load.recovery
  fi

  if [[ -f ${dest_dir}/modules.order.charger ]]; then
    run_depmod ${dest_stage} "${depmod_flags}" "${version}" "${dest_dir}/modules.order.charger"
    cp ${dest_dir}/modules.order.charger ${dest_dir}/modules.load.charger
  fi

  # Re-run depmod to detect any dependencies between in-kernel and external
  # modules. Then, create modules.order based on all the modules compiled.
  run_depmod ${dest_stage} "${depmod_flags}" "${version}"
  cp ${dest_dir}/modules.order ${dest_dir}/modules.load
}

function build_system_dlkm() {
  echo "========================================================"
  echo " Creating system_dlkm image"

  rm -rf ${SYSTEM_DLKM_STAGING_DIR}
  # MODULES_[RECOVERY_LIST|CHARGER]_LIST should not influence system_dlkm, as
  # GKI modules are not loaded when booting into either recovery or charger
  # modes, so do not consider them, and pass empty strings instead.
  create_modules_staging "${SYSTEM_DLKM_MODULES_LIST:-${MODULES_LIST}}" "${MODULES_STAGING_DIR}" \
    ${SYSTEM_DLKM_STAGING_DIR} "${SYSTEM_DLKM_MODULES_BLOCKLIST:-${MODULES_BLOCKLIST}}" \
    "" "" "-e"

  local system_dlkm_root_dir=$(echo ${SYSTEM_DLKM_STAGING_DIR}/lib/modules/*)
  cp ${system_dlkm_root_dir}/modules.load ${DIST_DIR}/system_dlkm.modules.load
  local system_dlkm_props_file

  if [ -z "${SYSTEM_DLKM_PROPS}" ]; then
    system_dlkm_props_file="$(mktemp)"
    echo -e "system_dlkm_fs_type=ext4\n" >> ${system_dlkm_props_file}
    echo -e "use_dynamic_partition_size=true\n" >> ${system_dlkm_props_file}
    echo -e "ext_mkuserimg=mkuserimg_mke2fs\n" >> ${system_dlkm_props_file}
    echo -e "ext4_share_dup_blocks=true\n" >> ${system_dlkm_props_file}
  else
    system_dlkm_props_file="${SYSTEM_DLKM_PROPS}"
    if [[ -f "${ROOT_DIR}/${system_dlkm_props_file}" ]]; then
      system_dlkm_props_file="${ROOT_DIR}/${system_dlkm_props_file}"
    elif [[ "${system_dlkm_props_file}" != /* ]]; then
      echo "SYSTEM_DLKM_PROPS must be an absolute path or relative to ${ROOT_DIR}: ${system_dlkm_props_file}"
      exit 1
    elif [[ ! -f "${system_dlkm_props_file}" ]]; then
      echo "Failed to find SYSTEM_DLKM_PROPS: ${system_dlkm_props_file}"
      exit 1
    fi
  fi

  # Re-sign the stripped modules using kernel build time key
  # If SYSTEM_DLKM_RE_SIGN=0, this is a trick in Kleaf for building
  # device-specific system_dlkm image, where keys are not available but the
  # signed and stripped modules are in MODULES_STAGING_DIR.
  if [[ ${SYSTEM_DLKM_RE_SIGN:-1} == "1" ]]; then
    for module in $(find ${SYSTEM_DLKM_STAGING_DIR} -type f -name "*.ko"); do
      ${OUT_DIR}/scripts/sign-file sha1 \
      ${OUT_DIR}/certs/signing_key.pem \
      ${OUT_DIR}/certs/signing_key.x509 "${module}"
    done
  fi

  build_image "${SYSTEM_DLKM_STAGING_DIR}" "${system_dlkm_props_file}" \
    "${DIST_DIR}/system_dlkm.img" /dev/null

  # No need to sign the image as modules are signed
  avbtool add_hashtree_footer \
    --partition_name system_dlkm \
    --hash_algorithm sha256 \
    --image "${DIST_DIR}/system_dlkm.img"

  # Archive system_dlkm_staging_dir
  tar -czf "${DIST_DIR}/system_dlkm_staging_archive.tar.gz" -C "${SYSTEM_DLKM_STAGING_DIR}" .
}

function build_vendor_dlkm() {
  echo "========================================================"
  echo " Creating vendor_dlkm image"

  create_modules_staging "${VENDOR_DLKM_MODULES_LIST}" "${MODULES_STAGING_DIR}" \
    "${VENDOR_DLKM_STAGING_DIR}" "${VENDOR_DLKM_MODULES_BLOCKLIST}"

  local vendor_dlkm_modules_root_dir=$(echo ${VENDOR_DLKM_STAGING_DIR}/lib/modules/*)
  local vendor_dlkm_modules_load=${vendor_dlkm_modules_root_dir}/modules.load
  if [ -f ${vendor_dlkm_modules_root_dir}/modules.blocklist ]; then
    cp ${vendor_dlkm_modules_root_dir}/modules.blocklist ${DIST_DIR}/vendor_dlkm.modules.blocklist
  fi

  # Modules loaded in vendor_boot should not be loaded in vendor_dlkm.
  if [ -f ${DIST_DIR}/modules.load ]; then
    local stripped_modules_load="$(mktemp)"
    ! grep -x -v -F -f ${DIST_DIR}/modules.load \
      ${vendor_dlkm_modules_load} > ${stripped_modules_load}
    mv -f ${stripped_modules_load} ${vendor_dlkm_modules_load}
  fi

  cp ${vendor_dlkm_modules_load} ${DIST_DIR}/vendor_dlkm.modules.load

  if [ -e ${vendor_dlkm_modules_root_dir}/modules.blocklist ]; then
    cp ${vendor_dlkm_modules_root_dir}/modules.blocklist \
      ${DIST_DIR}/vendor_dlkm.modules.blocklist
  fi

  local vendor_dlkm_props_file

  if [ -z "${VENDOR_DLKM_PROPS}" ]; then
    vendor_dlkm_props_file="$(mktemp)"
    echo -e "vendor_dlkm_fs_type=ext4\n" >> ${vendor_dlkm_props_file}
    echo -e "use_dynamic_partition_size=true\n" >> ${vendor_dlkm_props_file}
    echo -e "ext_mkuserimg=mkuserimg_mke2fs\n" >> ${vendor_dlkm_props_file}
    echo -e "ext4_share_dup_blocks=true\n" >> ${vendor_dlkm_props_file}
  else
    vendor_dlkm_props_file="${VENDOR_DLKM_PROPS}"
    if [[ -f "${ROOT_DIR}/${vendor_dlkm_props_file}" ]]; then
      vendor_dlkm_props_file="${ROOT_DIR}/${vendor_dlkm_props_file}"
    elif [[ "${vendor_dlkm_props_file}" != /* ]]; then
      echo "VENDOR_DLKM_PROPS must be an absolute path or relative to ${ROOT_DIR}: ${vendor_dlkm_props_file}"
      exit 1
    elif [[ ! -f "${vendor_dlkm_props_file}" ]]; then
      echo "Failed to find VENDOR_DLKM_PROPS: ${vendor_dlkm_props_file}"
      exit 1
    fi
  fi
  build_image "${VENDOR_DLKM_STAGING_DIR}" "${vendor_dlkm_props_file}" \
    "${DIST_DIR}/vendor_dlkm.img" /dev/null

  avbtool add_hashtree_footer \
    --partition_name vendor_dlkm \
    --hash_algorithm sha256 \
    --image "${DIST_DIR}/vendor_dlkm.img"
}

function check_mkbootimg_path() {
  if [ -z "${MKBOOTIMG_PATH}" ]; then
    MKBOOTIMG_PATH="tools/mkbootimg/mkbootimg.py"
  fi
  if [ ! -f "${MKBOOTIMG_PATH}" ]; then
    echo "mkbootimg.py script not found. MKBOOTIMG_PATH = ${MKBOOTIMG_PATH}"
    exit 1
  fi
}

function build_boot_images() {
  check_mkbootimg_path

  BOOT_IMAGE_HEADER_VERSION=${BOOT_IMAGE_HEADER_VERSION:-3}
  MKBOOTIMG_ARGS=("--header_version" "${BOOT_IMAGE_HEADER_VERSION}")
  if [ -n  "${BASE_ADDRESS}" ]; then
    MKBOOTIMG_ARGS+=("--base" "${BASE_ADDRESS}")
  fi
  if [ -n  "${PAGE_SIZE}" ]; then
    MKBOOTIMG_ARGS+=("--pagesize" "${PAGE_SIZE}")
  fi
  if [ -n "${KERNEL_VENDOR_CMDLINE}" -a "${BOOT_IMAGE_HEADER_VERSION}" -lt "3" ]; then
    KERNEL_CMDLINE+=" ${KERNEL_VENDOR_CMDLINE}"
  fi
  if [ -n "${KERNEL_CMDLINE}" ]; then
    MKBOOTIMG_ARGS+=("--cmdline" "${KERNEL_CMDLINE}")
  fi
  if [ -n "${TAGS_OFFSET}" ]; then
    MKBOOTIMG_ARGS+=("--tags_offset" "${TAGS_OFFSET}")
  fi
  if [ -n "${RAMDISK_OFFSET}" ]; then
    MKBOOTIMG_ARGS+=("--ramdisk_offset" "${RAMDISK_OFFSET}")
  fi

  DTB_FILE_LIST=$(find ${DIST_DIR} -name "*.dtb" | sort)
  if [ -z "${DTB_FILE_LIST}" ]; then
    if [ -z "${SKIP_VENDOR_BOOT}" ]; then
      echo "No *.dtb files found in ${DIST_DIR}"
      exit 1
    fi
  else
    cat $DTB_FILE_LIST > ${DIST_DIR}/dtb.img
    MKBOOTIMG_ARGS+=("--dtb" "${DIST_DIR}/dtb.img")
  fi

  rm -rf "${MKBOOTIMG_STAGING_DIR}"
  MKBOOTIMG_RAMDISK_STAGING_DIR="${MKBOOTIMG_STAGING_DIR}/ramdisk_root"
  mkdir -p "${MKBOOTIMG_RAMDISK_STAGING_DIR}"

  if [ -z "${SKIP_UNPACKING_RAMDISK}" ]; then
    if [ -n "${VENDOR_RAMDISK_BINARY}" ]; then
      VENDOR_RAMDISK_CPIO="${MKBOOTIMG_STAGING_DIR}/vendor_ramdisk_binary.cpio"
      rm -f "${VENDOR_RAMDISK_CPIO}"
      for vendor_ramdisk_binary in ${VENDOR_RAMDISK_BINARY}; do
        if ! [ -f "${vendor_ramdisk_binary}" ]; then
          echo "Unable to locate vendor ramdisk ${vendor_ramdisk_binary}."
          exit 1
        fi
        if ${DECOMPRESS_GZIP} "${vendor_ramdisk_binary}" 2>/dev/null >> "${VENDOR_RAMDISK_CPIO}"; then
          echo "${vendor_ramdisk_binary} is GZIP compressed"
        elif ${DECOMPRESS_LZ4} "${vendor_ramdisk_binary}" 2>/dev/null >> "${VENDOR_RAMDISK_CPIO}"; then
          echo "${vendor_ramdisk_binary} is LZ4 compressed"
        elif cpio -t < "${vendor_ramdisk_binary}" &>/dev/null; then
          echo "${vendor_ramdisk_binary} is plain CPIO archive"
          cat "${vendor_ramdisk_binary}" >> "${VENDOR_RAMDISK_CPIO}"
        else
          echo "Unable to identify type of vendor ramdisk ${vendor_ramdisk_binary}"
          rm -f "${VENDOR_RAMDISK_CPIO}"
          exit 1
        fi
      done

      # Remove lib/modules from the vendor ramdisk binary
      # Also execute ${VENDOR_RAMDISK_CMDS} for further modifications
      ( cd "${MKBOOTIMG_RAMDISK_STAGING_DIR}"
        cpio -idu --quiet <"${VENDOR_RAMDISK_CPIO}"
        rm -rf lib/modules
        eval ${VENDOR_RAMDISK_CMDS}
      )
    fi

  fi

  if [ -f "${VENDOR_FSTAB}" ]; then
    mkdir -p "${MKBOOTIMG_RAMDISK_STAGING_DIR}/first_stage_ramdisk"
    cp "${VENDOR_FSTAB}" "${MKBOOTIMG_RAMDISK_STAGING_DIR}/first_stage_ramdisk/"
  fi

  HAS_RAMDISK=
  MKBOOTIMG_RAMDISK_DIRS=()
  if [ -n "${VENDOR_RAMDISK_BINARY}" ] || [ -f "${VENDOR_FSTAB}" ]; then
    HAS_RAMDISK="1"
    MKBOOTIMG_RAMDISK_DIRS+=("${MKBOOTIMG_RAMDISK_STAGING_DIR}")
  fi

  if [ "${BUILD_INITRAMFS}" = "1" ]; then
    HAS_RAMDISK="1"
    if [ -z "${INITRAMFS_VENDOR_RAMDISK_FRAGMENT_NAME}" ]; then
      MKBOOTIMG_RAMDISK_DIRS+=("${INITRAMFS_STAGING_DIR}")
    fi
  fi

  if [ -z "${HAS_RAMDISK}" ] && [ -z "${SKIP_VENDOR_BOOT}" ]; then
    echo "No ramdisk found. Please provide a GKI and/or a vendor ramdisk."
    exit 1
  fi

  if [ -n "${SKIP_UNPACKING_RAMDISK}" ] && [ -e "${VENDOR_RAMDISK_BINARY}" ]; then
    cp "${VENDOR_RAMDISK_BINARY}" "${DIST_DIR}/ramdisk.${RAMDISK_EXT}"
  elif [ "${#MKBOOTIMG_RAMDISK_DIRS[@]}" -gt 0 ]; then
    MKBOOTIMG_RAMDISK_CPIO="${MKBOOTIMG_STAGING_DIR}/ramdisk.cpio"
    mkbootfs "${MKBOOTIMG_RAMDISK_DIRS[@]}" >"${MKBOOTIMG_RAMDISK_CPIO}"
    ${RAMDISK_COMPRESS} "${MKBOOTIMG_RAMDISK_CPIO}" >"${DIST_DIR}/ramdisk.${RAMDISK_EXT}"
  fi

  if [ -n "${BUILD_BOOT_IMG}" ]; then
    if [ ! -f "${DIST_DIR}/$KERNEL_BINARY" ]; then
      echo "kernel binary(KERNEL_BINARY = $KERNEL_BINARY) not present in ${DIST_DIR}"
      exit 1
    fi
    MKBOOTIMG_ARGS+=("--kernel" "${DIST_DIR}/${KERNEL_BINARY}")
  fi

  if [ "${BOOT_IMAGE_HEADER_VERSION}" -ge "4" ] \
     && [ -n "${BUILD_INIT_BOOT_IMG}" ]; then
    INIT_BOOT_IMAGE_FILENAME="init_boot.img"
    MKINITBOOTIMG_ARGS+=("--output" "${DIST_DIR}/${INIT_BOOT_IMAGE_FILENAME}")
  fi

  if [ "${BOOT_IMAGE_HEADER_VERSION}" -ge "4" ]; then
    if [ -n "${VENDOR_BOOTCONFIG}" ]; then
      for PARAM in ${VENDOR_BOOTCONFIG}; do
        echo "${PARAM}"
      done >"${DIST_DIR}/vendor-bootconfig.img"
      MKBOOTIMG_ARGS+=("--vendor_bootconfig" "${DIST_DIR}/vendor-bootconfig.img")
      KERNEL_VENDOR_CMDLINE+=" bootconfig"
    fi
  fi

  if [ "${BOOT_IMAGE_HEADER_VERSION}" -ge "3" ]; then
    if [ -f "${GKI_RAMDISK_PREBUILT_BINARY}" ]; then
      if [ "${BOOT_IMAGE_HEADER_VERSION}" -ge "4" ] \
         && [ -n "${BUILD_INIT_BOOT_IMG}" ]; then
        MKINITBOOTIMG_ARGS+=("--ramdisk" "${GKI_RAMDISK_PREBUILT_BINARY}")
        MKINITBOOTIMG_ARGS+=("--header_version" "${BOOT_IMAGE_HEADER_VERSION}")
      else
        MKBOOTIMG_ARGS+=("--ramdisk" "${GKI_RAMDISK_PREBUILT_BINARY}")
      fi
    fi

    if [ "${BUILD_VENDOR_KERNEL_BOOT}" = "1" ]; then
      VENDOR_BOOT_NAME="vendor_kernel_boot.img"
    elif [ -z "${SKIP_VENDOR_BOOT}" ]; then
      VENDOR_BOOT_NAME="vendor_boot.img"
    fi
    if [ -n "${VENDOR_BOOT_NAME}" ]; then
      MKBOOTIMG_ARGS+=("--vendor_boot" "${DIST_DIR}/${VENDOR_BOOT_NAME}")
      if [ -n "${KERNEL_VENDOR_CMDLINE}" ]; then
        MKBOOTIMG_ARGS+=("--vendor_cmdline" "${KERNEL_VENDOR_CMDLINE}")
      fi
      if [ -f "${DIST_DIR}/ramdisk.${RAMDISK_EXT}" ]; then
        MKBOOTIMG_ARGS+=("--vendor_ramdisk" "${DIST_DIR}/ramdisk.${RAMDISK_EXT}")
      fi
      if [ "${BUILD_INITRAMFS}" = "1" ] \
          && [ -n "${INITRAMFS_VENDOR_RAMDISK_FRAGMENT_NAME}" ]; then
        MKBOOTIMG_ARGS+=("--ramdisk_type" "DLKM")
        for MKBOOTIMG_ARG in ${INITRAMFS_VENDOR_RAMDISK_FRAGMENT_MKBOOTIMG_ARGS}; do
          MKBOOTIMG_ARGS+=("${MKBOOTIMG_ARG}")
        done
        MKBOOTIMG_ARGS+=("--ramdisk_name" "${INITRAMFS_VENDOR_RAMDISK_FRAGMENT_NAME}")
        MKBOOTIMG_ARGS+=("--vendor_ramdisk_fragment" "${DIST_DIR}/initramfs.img")
      fi
    fi
  else
    if [ -f "${DIST_DIR}/ramdisk.${RAMDISK_EXT}" ]; then
      MKBOOTIMG_ARGS+=("--ramdisk" "${DIST_DIR}/ramdisk.${RAMDISK_EXT}")
    fi
  fi

  if [ -z "${BOOT_IMAGE_FILENAME}" ]; then
    BOOT_IMAGE_FILENAME="boot.img"
  fi
  if [ -n "${BUILD_BOOT_IMG}" ]; then
    MKBOOTIMG_ARGS+=("--output" "${DIST_DIR}/${BOOT_IMAGE_FILENAME}")
  fi

  for MKBOOTIMG_ARG in ${MKBOOTIMG_EXTRA_ARGS}; do
    MKBOOTIMG_ARGS+=("${MKBOOTIMG_ARG}")
  done

  "${MKBOOTIMG_PATH}" "${MKBOOTIMG_ARGS[@]}"

  if [ "${BOOT_IMAGE_HEADER_VERSION}" -ge "4" ] \
     && [ -n "${BUILD_INIT_BOOT_IMG}" ]; then
    "${MKBOOTIMG_PATH}" "${MKINITBOOTIMG_ARGS[@]}"
  fi

  if [ -n "${BUILD_INIT_BOOT_IMG}" -a -f "${DIST_DIR}/${INIT_BOOT_IMAGE_FILENAME}" ]; then
    echo "init_boot image created at ${DIST_DIR}/${INIT_BOOT_IMAGE_FILENAME}"
  fi

  if [ -n "${BUILD_BOOT_IMG}" -a -f "${DIST_DIR}/${BOOT_IMAGE_FILENAME}" ]; then
    echo "boot image created at ${DIST_DIR}/${BOOT_IMAGE_FILENAME}"

    if [ -n "${AVB_SIGN_BOOT_IMG}" ]; then
      if [ -n "${AVB_BOOT_PARTITION_SIZE}" ] \
          && [ -n "${AVB_BOOT_KEY}" ] \
          && [ -n "${AVB_BOOT_ALGORITHM}" ]; then
        echo "Signing ${BOOT_IMAGE_FILENAME}..."

        if [ -z "${AVB_BOOT_PARTITION_NAME}" ]; then
          AVB_BOOT_PARTITION_NAME=${BOOT_IMAGE_FILENAME%%.*}
        fi

        avb_sign_args=("add_hash_footer")
        avb_sign_args+=("--partition_name" "${AVB_BOOT_PARTITION_NAME}")
        avb_sign_args+=("--partition_size" "${AVB_BOOT_PARTITION_SIZE}")
        avb_sign_args+=("--image" "${DIST_DIR}/${BOOT_IMAGE_FILENAME}")
        avb_sign_args+=("--algorithm" "${AVB_BOOT_ALGORITHM}")
        avb_sign_args+=("--key" "${AVB_BOOT_KEY}")

        for prop in "${AVB_SIGN_BOOT_IMG_PROP[@]}"; do
          avb_sign_args+=("--prop" "${prop}")
        done
        avbtool "${avb_sign_args[@]}"
      else
        echo "Missing the AVB_* flags. Failed to sign the boot image" 1>&2
        exit 1
      fi
    fi
  fi

  if [ -z "${SKIP_VENDOR_BOOT}" ] \
    && [ "${BOOT_IMAGE_HEADER_VERSION}" -ge "3" ] \
    && [ -f "${DIST_DIR}/${VENDOR_BOOT_NAME}" ]; then
      echo "Created ${VENDOR_BOOT_NAME} at ${DIST_DIR}/${VENDOR_BOOT_NAME}"
  fi
}

function make_dtbo() {
  echo "========================================================"
  echo " Creating dtbo image at ${DIST_DIR}/dtbo.img"
  (
    cd ${OUT_DIR}
    mkdtimg create "${DIST_DIR}"/dtbo.img ${MKDTIMG_FLAGS} ${MKDTIMG_DTBOS}
  )
}

# gki_get_boot_img_size <compression method>.
# The function echoes the value of the preconfigured size variable
# based on the input compression method.
#   - (empty): echo ${BUILD_GKI_BOOT_IMG_SIZE}
#   -      gz: echo ${BUILD_GKI_BOOT_IMG_GZ_SIZE}
#   -     lz4: echo ${BUILD_GKI_BOOT_IMG_LZ4_SIZE}
function gki_get_boot_img_size() {
  local compression

  if [ -z "$1" ]; then
    boot_size_var="BUILD_GKI_BOOT_IMG_SIZE"
  else
    compression=$(echo "$1" | tr '[:lower:]' '[:upper:]')
    boot_size_var="BUILD_GKI_BOOT_IMG_${compression}_SIZE"
  fi

  if [ -z "${!boot_size_var}" ]; then
    echo "ERROR: ${boot_size_var} is not set." >&2
    exit 1
  fi

  echo "${!boot_size_var}"
}

# gki_add_avb_footer <image> <partition_size> <security_patch_level>
function gki_add_avb_footer() {
  local spl_date="$3"
  local additional_props=""
  if [ -n "${spl_date}" ]; then
    additional_props="--prop com.android.build.boot.security_patch:${spl_date}"
  fi

  avbtool add_hash_footer --image "$1" \
    --partition_name boot --partition_size "$2" \
    ${additional_props}
}

# gki_dry_run_certify_bootimg <boot_image> <gki_artifacts_info_file> <security_patch_level>
# The certify_bootimg script will be executed on a server over a GKI
# boot.img during the official certification process, which embeds
# a GKI certificate into the boot.img. The certificate is for Android
# VTS to verify that a GKI boot.img is authentic.
# Dry running the process here so we can catch related issues early.
function gki_dry_run_certify_bootimg() {
  local spl_date="$3"
  local additional_props=()
  if [ -n "${spl_date}" ]; then
    additional_props+=("--extra_footer_args" \
      "--prop com.android.build.boot.security_patch:${spl_date}")
  fi

  certify_bootimg --boot_img "$1" \
    --algorithm SHA256_RSA4096 \
    --key tools/mkbootimg/gki/testdata/testkey_rsa4096.pem \
    --gki_info "$2" \
    --output "$1" \
    "${additional_props[@]}"
}

# build_gki_artifacts_info <output_gki_artifacts_info_file>
function build_gki_artifacts_info() {
  local artifacts_info="certify_bootimg_extra_args=--prop ARCH:${ARCH} --prop BRANCH:${BRANCH}"

  if [ -n "${BUILD_NUMBER}" ]; then
    artifacts_info="${artifacts_info} --prop BUILD_NUMBER:${BUILD_NUMBER}"
  fi

  KERNEL_RELEASE="$(cat "${OUT_DIR}"/include/config/kernel.release)"
  artifacts_info="${artifacts_info} --prop KERNEL_RELEASE:${KERNEL_RELEASE}"

  echo "${artifacts_info}" > "$1"
}

# build_gki_boot_images <uncompressed kernel path>.
# The function builds boot-*.img for kernel images
# with the prefix of <uncompressed kernel path>.
# It also generates a boot-img.tar.gz containing those
# boot-*.img files. The uncompressed kernel image should
# exist, e.g., ${DIST_DIR}/Image, while other compressed
# kernel images are optional, e.g., ${DIST_DIR}/Image.gz.
function build_gki_boot_images() {
  local uncompressed_kernel_path=$1
  # Pick a SPL date far enough in the future so that you can flash
  # development GKI kernels on an unlocked device without wiping the
  # userdata. This is for development purposes only and should be
  # overwritten by the Android platform build to include an accurate SPL.
  # Note, the certified GKI release builds will not include the SPL
  # property.
  local spl_month=$((($(date +'%-m') + 3) % 12))
  local spl_year="$(date +'%Y')"
  if [ $((${spl_month} % 3)) -gt 0 ]; then
    # Round up to the next quarterly platform release (QPR) month
    spl_month=$((${spl_month} + 3 - (${spl_month} % 3)))
  fi
  if [ "${spl_month}" -lt "$(date +'%-m')" ]; then
    # rollover to the next year
    spl_year="$((${spl_year} + 1))"
  fi
  local spl_date=$(printf "%d-%02d-05\n" ${spl_year} ${spl_month})

  if ! [ -f "${uncompressed_kernel_path}" ]; then
    echo "ERROR: '${uncompressed_kernel_path}' doesn't exist" >&2
    exit 1
  fi

  uncompressed_kernel_image="$(basename "${uncompressed_kernel_path}")"

  DEFAULT_MKBOOTIMG_ARGS=("--header_version" "4")
  if [ -n "${GKI_KERNEL_CMDLINE}" ]; then
    DEFAULT_MKBOOTIMG_ARGS+=("--cmdline" "${GKI_KERNEL_CMDLINE}")
  fi

  GKI_ARTIFACTS_INFO_FILE="${DIST_DIR}/gki-info.txt"
  build_gki_artifacts_info "${GKI_ARTIFACTS_INFO_FILE}"
  local images_to_pack=("$(basename "${GKI_ARTIFACTS_INFO_FILE}")")

  # Compressed kernel images, e.g., Image.gz, Image.lz4 have the same
  # prefix as the uncompressed kernel image, e.g., Image.
  for kernel_path in "${uncompressed_kernel_path}"*; do
    GKI_MKBOOTIMG_ARGS=("${DEFAULT_MKBOOTIMG_ARGS[@]}")
    GKI_MKBOOTIMG_ARGS+=("--kernel" "${kernel_path}")

    if [ "${kernel_path}" = "${uncompressed_kernel_path}" ]; then
        boot_image="boot.img"
    else
        kernel_image="$(basename "${kernel_path}")"
        compression="${kernel_image#"${uncompressed_kernel_image}".}"
        boot_image="boot-${compression}.img"
    fi

    boot_image_path="${DIST_DIR}/${boot_image}"
    GKI_MKBOOTIMG_ARGS+=("--output" "${boot_image_path}")
    "${MKBOOTIMG_PATH}" "${GKI_MKBOOTIMG_ARGS[@]}"

    gki_add_avb_footer "${boot_image_path}" \
      "$(gki_get_boot_img_size "${compression}")" "${spl_date}"
    gki_dry_run_certify_bootimg "${boot_image_path}" \
      "${GKI_ARTIFACTS_INFO_FILE}" "${spl_date}"
    images_to_pack+=("${boot_image}")
  done

  GKI_BOOT_IMG_ARCHIVE="boot-img.tar.gz"
  echo "Creating ${GKI_BOOT_IMG_ARCHIVE} for" "${images_to_pack[@]}"
  tar -czf "${DIST_DIR}/${GKI_BOOT_IMG_ARCHIVE}" -C "${DIST_DIR}" \
    "${images_to_pack[@]}"
}

function build_gki_artifacts() {
  check_mkbootimg_path

  if [ "${ARCH}" = "arm64" ]; then
    build_gki_boot_images "${DIST_DIR}/Image"
  elif [ "${ARCH}" = "x86_64" ]; then
    build_gki_boot_images "${DIST_DIR}/bzImage"
  else
    echo "ERROR: unknown ARCH to BUILD_GKI_ARTIFACTS: '${ARCH}'" >&2
    exit 1
  fi
}

#
# vim:expandtab:shiftwidth=4:softtabstop=4:tabstop=4:
#
# lc_common.sh - This file contains functions to be used by most or all
#                Lustre cluster config scripts.
#
################################################################################

# Remote command 
REMOTE=${REMOTE:-"ssh -x -q"}
#REMOTE=${REMOTE:-"pdsh -S -R ssh -w"}
export REMOTE

# Lustre utilities
CMD_PATH=${CMD_PATH:-"/usr/sbin"}
MKFS=${MKFS:-"$CMD_PATH/mkfs.lustre"}
TUNEFS=${TUNEFS:-"$CMD_PATH/tunefs.lustre"}
LCTL=${LCTL:-"$CMD_PATH/lctl"}

EXPORT_PATH=${EXPORT_PATH:-"PATH=\$PATH:/sbin:/usr/sbin;"}

# Raid command path
RAID_CMD_PATH=${RAID_CMD_PATH:-"/sbin"}
MDADM=${MDADM:-"$RAID_CMD_PATH/mdadm"}

# Some scripts to be called
SCRIPTS_PATH=${CLUSTER_SCRIPTS_PATH:-"$(cd `dirname $0`; echo $PWD)"}
MODULE_CONFIG=${SCRIPTS_PATH}/lc_modprobe.sh
VERIFY_CLUSTER_NET=${SCRIPTS_PATH}/lc_net.sh
GEN_HB_CONFIG=${SCRIPTS_PATH}/lc_hb.sh
GEN_CLUMGR_CONFIG=${SCRIPTS_PATH}/lc_cluman.sh
SCRIPT_VERIFY_SRVIP=${SCRIPTS_PATH}/lc_servip.sh
SCRIPT_GEN_MONCF=${SCRIPTS_PATH}/lc_mon.sh
SCRIPT_CONFIG_MD=${SCRIPTS_PATH}/lc_md.sh
SCRIPT_CONFIG_LVM=${SCRIPTS_PATH}/lc_lvm.sh

# Variables of HA software
HBVER_HBV1="hbv1"                   # Heartbeat version 1
HBVER_HBV2="hbv2"                   # Heartbeat version 2
HATYPE_CLUMGR="cluman"              # Cluster Manager

# Configuration directories and files
HA_DIR=${HA_DIR:-"/etc/ha.d"}		# Heartbeat configuration directory
MON_DIR=${MON_DIR:-"/etc/mon"}		# mon configuration directory
CIB_DIR=${CIB_DIR:-"/var/lib/heartbeat/crm"}	# cib.xml directory

HA_CF=${HA_DIR}/ha.cf               # ha.cf file
HA_RES=${HA_DIR}/haresources        # haresources file
HA_CIB=${CIB_DIR}/cib.xml

CLUMAN_DIR="/etc"			        # CluManager configuration directory
CLUMAN_CONFIG=${CLUMAN_DIR}/cluster.xml

CLUMAN_TOOLS_PATH=${CLUMAN_TOOLS_PATH:-"/usr/sbin"}	# CluManager tools
CONFIG_CMD=${CONFIG_CMD:-"${CLUMAN_TOOLS_PATH}/redhat-config-cluster-cmd"}

HB_TMP_DIR="/tmp/heartbeat"         # Temporary directory
CLUMGR_TMP_DIR="/tmp/clumanager"
TMP_DIRS="${HB_TMP_DIR} ${CLUMGR_TMP_DIR}"

FS_TYPE=${FS_TYPE:-"lustre"}        # Lustre filesystem type
FILE_SUFFIX=${FILE_SUFFIX:-".lustre"}	# Suffix of the generated config files

# Marker of the MD device line
MD_MARKER=${MD_MARKER:-"MD"}

# Marker of the LVM device line
PV_MARKER=${PV_MARKER:-"PV"}
VG_MARKER=${VG_MARKER:-"VG"}
LV_MARKER=${LV_MARKER:-"LV"}

declare -a CONFIG_ITEM              # Items in each line of the csv file
declare -a NODE_NAME                # Hostnames of nodes have been configured

# Nodelist variables
USE_ALLNODES=false                  # default is not to operate on all the nodes
SPECIFIED_NODELIST=""               # specified list of nodes to be operated on
EXCLUDED_NODELIST=""                # list of nodes to be excluded

export PATH=$PATH:$CMD_PATH:$SCRIPTS_PATH:$CLUMAN_TOOLS_PATH:$RAID_CMD_PATH:/sbin:/usr/sbin


# verbose_output string
# Output verbose information $string
verbose_output() {
    if ${VERBOSE_OUTPUT}; then
        echo "`basename $0`: $*"
    fi
    return 0
}

# Check whether the reomte command is pdsh
is_pdsh() {
    if [ "${REMOTE}" = "${REMOTE#*pdsh}" ]; then
        return 1
    fi

    return 0
}

# check_file csv_file
# Check the file $csv_file
check_file() {
    # Check argument
    if [ $# -eq 0 ]; then
        echo >&2 "`basename $0`: check_file() error: Missing csv file!"
        return 1
    fi

    CSV_FILE=$1
    if [ ! -s ${CSV_FILE} ]; then
        echo >&2 "`basename $0`: check_file() error: ${CSV_FILE}"\
                 "does not exist or is empty!"
        return 1
    fi

    return 0
}

# parse_line line
# Parse a line in the csv file
parse_line() {
    # Check argument
    if [ $# -eq 0 ]; then
        echo >&2 "`basename $0`: parse_line() error: Missing argument!"
        return 1
    fi

    declare -i i=0              # Index of the CONFIG_ITEM array
    declare -i length=0 
    declare -i idx=0
    declare -i s_quote_flag=0   # Flag of the single quote character 
    declare -i d_quote_flag=0   # Flag of the double quotes character
    local TMP_LETTER LINE
 
    LINE="$*"

    # Initialize the CONFIG_ITEM array
    unset CONFIG_ITEM

    # Get the length of the line
    length=${#LINE}

    i=0
    while [ ${idx} -lt ${length} ]; do
        # Get a letter from the line
        TMP_LETTER=${LINE:${idx}:1}

        case "${TMP_LETTER}" in
        ",")
            if [ ${s_quote_flag} -eq 1 -o ${d_quote_flag} -eq 1 ]
            then
                CONFIG_ITEM[i]=${CONFIG_ITEM[i]}${TMP_LETTER}
            else
                i=$i+1
            fi
            idx=${idx}+1
            continue
            ;;
        "'")
            if [ ${s_quote_flag} -eq 0 ]; then
                s_quote_flag=1
            else
                s_quote_flag=0
            fi
            ;;
        "\"")
            if [ ${d_quote_flag} -eq 0 ]; then
                d_quote_flag=1
            else
                d_quote_flag=0
            fi
            ;;
        "")
            idx=${idx}+1
            continue
            ;;
        *)
            ;;
        esac
        CONFIG_ITEM[i]=${CONFIG_ITEM[i]}${TMP_LETTER}
        idx=${idx}+1
    done

    # Extract the real value of each field
    # Remove surrounded double-quotes, etc.
    for ((idx = 0; idx <= $i; idx++)); do
        # Strip the leading and trailing space-characters
        CONFIG_ITEM[idx]=`expr "${CONFIG_ITEM[idx]}" : '[[:space:]]*\(.*\)[[:space:]]*$'`

        [ -z "${CONFIG_ITEM[idx]}" ] && continue

        # Remove the surrounded double-quotes
        while [ -z "`echo "${CONFIG_ITEM[idx]}"|sed -e 's/^".*"$//'`" ]; do
            CONFIG_ITEM[idx]=`echo "${CONFIG_ITEM[idx]}" | sed -e 's/^"//' -e 's/"$//'`
        done

        CONFIG_ITEM[idx]=`echo "${CONFIG_ITEM[idx]}" | sed -e 's/""/"/g'`
    done

    return 0
}

# fcanon name
# If $name is a symbolic link, then display it's value
fcanon() {
    local NAME=$1

    if [ -h "$NAME" ]; then
        readlink -f "$NAME"
    else
        echo "$NAME"
    fi
}

# configured_host host_name
#
# Check whether the devices in $host_name has been configured or not
configured_host() {
    local host_name=$1
    declare -i i

    for ((i = 0; i < ${#NODE_NAME[@]}; i++)); do
        [ "${host_name}" = "${NODE_NAME[i]}" ] && return 0
    done

    return 1
}

# remote_error fn_name host_addr ret_str
# Verify the return result from remote command
remote_error() {
    local fn_name host_addr ret_str

    fn_name=$1
    shift
    host_addr=$1
    shift
    ret_str=$*

    if [ "${ret_str}" != "${ret_str#*connect:*}" ]; then
        echo >&2 "`basename $0`: ${fn_name}() error: ${ret_str}"
        return 0
    fi

    if [ -z "${ret_str}" ]; then
        echo >&2 "`basename $0`: ${fn_name}() error:" \
        "No results from remote!" \
        "Check network connectivity between the local host and ${host_addr}!"
        return 0
    fi

    return 1
}

# nid2hostname nid
# Convert $nid to hostname of the lustre cluster node
nid2hostname() {
    local nid=$1
    local host_name=
    local addr nettype ip_addr
    local ret_str

    addr=${nid%@*}
    [ "${nid}" != "${nid#*@*}" ] && nettype=${nid#*@} || nettype=tcp
    if [ -z "${addr}" ]; then
        echo "`basename $0`: nid2hostname() error: Invalid nid - \"${nid}\"!"
        return 1
    fi
		
    case "${nettype}" in
    lo*)    host_name=`hostname`;;
    elan*)  # QsNet
            # FIXME: Parse the /etc/elanhosts configuration file to
            # convert ElanID to hostname
            ;;
    gm*)    # Myrinet
            # FIXME: Use /usr/sbin/gmlndnid to find the hostname of
            # the specified GM Global node ID 
            ;;
    ptl*)   # Portals
            # FIXME: Convert portal ID to hostname
            ;;
    *)  # tcp, o2ib, cib, openib, iib, vib, ra
        ip_addr=${addr}
        # Is it IP address or hostname?
        if [ -n "`echo ${ip_addr} | sed -e 's/\([0-9]\{1,3\}\.\)\{3,3\}[0-9]\{1,3\}//'`" ]
        then
            host_name=${ip_addr}
            echo ${host_name}
            return 0
        fi

        # Execute remote command to get the host name
        ret_str=$(${REMOTE} ${ip_addr} "hostname" 2>&1)
        if [ ${PIPESTATUS[0]} -ne 0 -a -n "${ret_str}" ]; then
            echo "`basename $0`: nid2hostname() error:" \
            "remote command to ${ip_addr} error: ${ret_str}"
            return 1
        fi
        remote_error "nid2hostname" ${ip_addr} "${ret_str}" && return 1

        if is_pdsh; then
            host_name=`echo ${ret_str} | awk '{print $2}'`
        else
            host_name=`echo ${ret_str} | awk '{print $1}'`
        fi
        ;;
    esac

    echo ${host_name}
    return 0
}

# nids2hostname nids
# Get the hostname of the lustre cluster node which has the nids - $nids
nids2hostname() {
    local nids=$1
    local host_name=
    local nid
    local nettype

    for nid in ${nids//,/ }; do
        [ "${nid}" != "${nid#*@*}" ] && nettype=${nid#*@} || nettype=tcp

        case "${nettype}" in
        lo* | elan* | gm* | ptl*) ;;
        *)  # tcp, o2ib, cib, openib, iib, vib, ra
            host_name=$(nid2hostname ${nid})
            if [ ${PIPESTATUS[0]} -ne 0 ]; then
                echo "${host_name}"
                return 1
            fi
            ;;
        esac
    done

    if [ -z "${host_name}" ]; then
        echo "`basename $0`: nids2hostname() error:" \
        "Can not get the hostname from nids - \"${nids}\"!"
        return 1
    fi

    echo ${host_name}
    return 0
}

# ip2hostname_single_node nids
# Convert IP addresses in $nids into hostnames
# NID in $nids are delimited by commas, ie all the $nids belong to one node
ip2hostname_single_node() {
    local orig_nids=$1
    local nids=
    local nid host_name
    local nettype

    for nid in ${orig_nids//,/ }; do
        [ "${nid}" != "${nid#*@*}" ] && nettype=${nid#*@} || nettype=tcp

        case "${nettype}" in
        lo* | elan* | gm* | ptl*) ;;
        *)  # tcp, o2ib, cib, openib, iib, vib, ra
            host_name=$(nid2hostname ${nid})
            if [ ${PIPESTATUS[0]} -ne 0 ]; then
                echo "${host_name}"
                return 1
            fi
			
            nid=${host_name}@${nettype}
            ;;
        esac

        [ -z "${nids}" ] && nids=${nid} || nids=${nids},${nid}
    done

    echo ${nids}
    return 0
}

# ip2hostname_multi_node nids
# Convert IP addresses in $nids into hostnames
# NIDs belong to multiple nodes are delimited by colons in $nids
ip2hostname_multi_node() {
    local orig_nids=$1
    local nids=
    local nid

    for nid in ${orig_nids//:/ }; do
        nid=$(ip2hostname_single_node ${nid})
        if [ ${PIPESTATUS[0]} -ne 0 ]; then
            echo "${nid}"
            return 1
        fi

        [ -z "${nids}" ] && nids=${nid} || nids=${nids}:${nid}
    done

    echo ${nids}
    return 0
}

# comma_list space-delimited-list
# Convert a space-delimited list to a sorted list of unique values
# separated by commas.
comma_list() {
    # the sed converts spaces to commas, but leaves the last space
    # alone, so the line doesn't end with a comma.
    echo "$*" | tr -s " " "\n" | sort -b -u | tr "\n" " " | sed 's/ \([^$]\)/,\1/g'
}

# host_in_hostlist hostname hostlist
# Given a hostname, and a list of hostnames, return true if the hostname
# appears in the list of hostnames, or false otherwise.
host_in_hostlist() {
    local HOST=$1
    local HOSTLIST=$2

    [ -z "$HOST" -o -z "$HOSTLIST" ] && false && return

    # Hostnames in the list are separated by commas.
    [[ ,$HOSTLIST, == *,$HOST,* ]] && true && return

    false && return
}

# exclude_items_from_list list_of_items list_of_items_to_exclude
# Given a list of items, and a second list of items to exclude from
# the first list, return the contents of the first list minus the contents
# of the second.
exclude_items_from_list() {
    local INLIST=$1
    local EXCLUDELIST=$2
    local ITEM OUTLIST

    # Handle an empty inlist by throwing back an empty string.
    if [ -z "$INLIST" ]; then
        echo ""
        return 0
    fi

    # Handle an empty excludelist by throwing back the inlist unmodified.
    if [ -z "$EXCLUDELIST" ]; then
        echo $INLIST
        return 0
    fi

    for ITEM in ${INLIST//,/ }; do
        if ! host_in_hostlist $ITEM $EXCLUDELIST; then
           OUTLIST="$OUTLIST,$ITEM"
        fi
    done
                                
    # strip leading comma
    echo ${OUTLIST#,}
}

# get_csv_nodelist csv_file
# Get the comma-separated list of all the nodes from the csv file
get_csv_nodelist() {
    local csv_file=$1
    local all_nodelist

    # Check the csv file
    ! check_file ${csv_file} 2>&1 && return 1

    all_nodelist=$(egrep -v "([[:space:]]|^)#" ${csv_file} | cut -d, -f 1)
    all_nodelist=$(comma_list ${all_nodelist})

    echo ${all_nodelist}
    return 0
}

# get_nodelist
# Get the comma-separated list of nodes to be operated on
# Note: CSV_FILE, USE_ALLNODES, SPECIFIED_NODELIST and EXCLUDED_NODELIST
# are global variables
get_nodelist() {
    local ALL_NODELIST

    # Get the list of all the nodes in the csv file
    ALL_NODELIST=$(get_csv_nodelist ${CSV_FILE})
    [ ${PIPESTATUS[0]} -ne 0 ] && echo "${ALL_NODELIST}" && return 1

    if [ -z "${ALL_NODELIST}" ]; then
        echo "`basename $0`: get_nodelist() error:"\
             "There are no hosts in the ${CSV_FILE} file!"
        return 1
    fi

    if ${USE_ALLNODES}; then
        echo ${ALL_NODELIST} && return 0
    fi

    if [ -n "${SPECIFIED_NODELIST}" ]; then
        echo $(exclude_items_from_list ${SPECIFIED_NODELIST} ${EXCLUDED_NODELIST})
        return 0
    fi

    if [ -n "${EXCLUDED_NODELIST}" ]; then
        echo $(exclude_items_from_list ${ALL_NODELIST} ${EXCLUDED_NODELIST})
        return 0
    fi

    # No hosts to be operated on
    echo ""
    return 0
}

# check_nodelist nodelist
# Given a list of nodes to be operated on, check whether the nodelist is 
# empty or not and output prompt message.
check_nodelist() {
    local nodes_to_use=$1

    if [ -z "${nodes_to_use}" ]; then
        echo "`basename $0`: There are no hosts to be operated on."\
             "Check the node selection options (-a, -w or -x)."
        usage
    else
        verbose_output "Operating on the following nodes: ${nodes_to_use}"
    fi

    return 0
}

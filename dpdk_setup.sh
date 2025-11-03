#!/bin/bash

#this file is to preconfig the configuration about dpdk, like driver, hugepage and so on.

# DPDK 自动环境配置脚本
# 使用方法: sudo ./dpdk_setup.sh [install|config|bind|status|cleanup]

set -e

# 配置变量
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${SCRIPT_DIR}/dpdk_config.conf"
LOG_FILE="${SCRIPT_DIR}/dpdk_setup.log"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 日志函数
log() {
    echo -e "${GREEN}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1" | tee -a "$LOG_FILE"
}

warn() {
    echo -e "${YELLOW}[WARNING] $1${NC}" | tee -a "$LOG_FILE"
}

error() {
    echo -e "${RED}[ERROR] $1${NC}" | tee -a "$LOG_FILE"
    exit 1
}

# 加载配置文件
load_config() {
    if [[ ! -f "$CONFIG_FILE" ]]; then
        error "配置文件 $CONFIG_FILE 不存在，请先创建"
    fi
    
    source "$CONFIG_FILE"
    log "配置文件加载成功"
}

# 检查root权限
check_root() {
    if [[ $EUID -ne 0 ]]; then
        error "此脚本必须以root权限运行"
    fi
}

# 配置大页内存
configure_hugepages() {
    log "配置大页内存..."
    
    # 卸载现有挂载
    umount /mnt/huge 2>/dev/null || true
    rm -rf /mnt/huge
    mkdir -p /mnt/huge
    
    # 配置2MB大页
    if [[ -n "$HUGEPAGES_2MB" ]] && [[ "$HUGEPAGES_2MB" -gt 0 ]]; then
        echo "echo $HUGEPAGES_2MB > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages" | tee -a "$LOG_FILE"
        echo "$HUGEPAGES_2MB" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    fi
    
    # 配置1GB大页
    if [[ -n "$HUGEPAGES_1GB" ]] && [[ "$HUGEPAGES_1GB" -gt 0 ]]; then
        echo "echo $HUGEPAGES_1GB > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages" | tee -a "$LOG_FILE"
        echo "$HUGEPAGES_1GB" > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
    fi
    
    # 挂载大页文件系统
    mount -t hugetlbfs nodev /mnt/huge
    
    # 验证配置
    log "大页内存配置:"
    cat /proc/meminfo | grep -i huge | tee -a "$LOG_FILE"
}

# 加载内核模块
load_kernel_modules() {
    log "加载内核模块..."
    
    # 尝试加载vfio-pci
    modprobe vfio 2>/dev/null || warn "VFIO模块加载失败"
    modprobe vfio-pci 2>/dev/null || warn "VFIO-PCI模块加载失败"
    
    # 加载uio作为备选
    modprobe uio 2>/dev/null || warn "UIO模块加载失败"
    
    # 编译并加载igb_uio
    if [[ -n "$RTE_SDK" ]] && [[ -d "$RTE_SDK" ]]; then
        local igb_uio_path=""
        if [[ -f "$RTE_SDK/build/kmod/igb_uio.ko" ]]; then
            igb_uio_path="$RTE_SDK/build/kmod/igb_uio.ko"
        elif [[ -f "$RTE_SDK/kmod/igb_uio.ko" ]]; then
            igb_uio_path="$RTE_SDK/kmod/igb_uio.ko"
        fi
        
        if [[ -n "$igb_uio_path" ]]; then
            insmod "$igb_uio_path" 2>/dev/null && log "igb_uio加载成功" || warn "igb_uio加载失败"
        fi
    fi
    
    # 启用IOMMU（如果内核支持）
    if [[ "$ENABLE_IOMMU" == "yes" ]]; then
        sed -i 's/GRUB_CMDLINE_LINUX="/&intel_iommu=on /' /etc/default/grub 2>/dev/null || true
        update-grub 2>/dev/null || grub2-mkconfig -o /boot/grub2/grub.cfg 2>/dev/null || true
        warn "已启用IOMMU，需要重启系统生效"
    fi
}

# 函数：获取MAC地址（只返回，不写入）
get_mac_address() {
    local pci=$1
    local mac_addr=""
    
    # 方法1: 从sysfs读取
    if [ -d "/sys/bus/pci/devices/$pci/net" ]; then
        local iface=$(ls /sys/bus/pci/devices/$pci/net/ 2>/dev/null | head -1)
        if [ -n "$iface" ] && [ -f "/sys/bus/pci/devices/$pci/net/$iface/address" ]; then
            mac_addr=$(cat "/sys/bus/pci/devices/$pci/net/$iface/address" | tr -d '[:space:]')
        fi
    fi
    
    # 方法2: 如果方法1失败，使用ip命令
    if [ -z "$mac_addr" ]; then
        mac_addr=$(ip -o link show 2>/dev/null | grep "$pci" | awk '{print $17}' | head -1 | tr -d '[:space:]')
    fi
    
    echo "$mac_addr"
}

write_mac_record() {
    file=$MAC_RECORD
    echo "# PCI_Address MAC_Address" > "$file"
    echo "# Generated on $(date)" >> "$file"

    if [[ -n "$BIND_NICS" ]]; then
        IFS=',' read -ra NIC_ARRAY <<< "$BIND_NICS"
        for nic in "${NIC_ARRAY[@]}"; do
            mac_addr=$(get_mac_address "$nic")
            echo $nic $mac_addr >> $file
        done
    fi

}

# 绑定网卡到DPDK驱动
bind_nics() {
    log "绑定网卡到DPDK驱动..."
    
    if [[ -z "$RTE_SDK" ]]; then
        error "RTE_SDK未设置，请先安装DPDK"
    fi
    
    local bind_tool="$RTE_SDK/usertools/dpdk-devbind.py"
    if [[ ! -f "$bind_tool" ]]; then
        bind_tool="$RTE_SDK/tools/dpdk-devbind.py"
    fi
    
    if [[ ! -f "$bind_tool" ]]; then
        error "找不到dpdk-devbind.py工具"
    fi
    
    # 显示当前网卡状态
    log "当前网卡状态:"
    python3 "$bind_tool" --status
    
    # 绑定指定的网卡
    if [[ -n "$BIND_NICS" ]]; then
        IFS=',' read -ra NIC_ARRAY <<< "$BIND_NICS"
        for nic in "${NIC_ARRAY[@]}"; do
            log "绑定网卡 $nic 到驱动 $DPDK_DRIVER..."
            
            # 先解绑当前驱动
            python3 "$bind_tool" -u "$nic" 2>/dev/null || warn "解绑网卡 $nic 失败"
            
            # 绑定到DPDK驱动
            if python3 "$bind_tool" -b "$DPDK_DRIVER" "$nic"; then
                log "网卡 $nic 绑定成功"
            else
                error "网卡 $nic 绑定失败"
            fi
        done
    fi
    
    log "绑定后的网卡状态:"
    python3 "$bind_tool" --status
}

# 显示状态
show_status() {
    log "=== DPDK环境状态检查 ==="
    
    echo -e "\n1. 大页内存状态:"
    cat /proc/meminfo | grep -i huge
    
    echo -e "\n2. 内核模块状态:"
    lsmod | grep -E "vfio|uio"
    
    echo -e "\n3. 网卡绑定状态:"
    if [[ -n "$RTE_SDK" ]] && [[ -f "$RTE_SDK/usertools/dpdk-devbind.py" ]]; then
        python3 "$RTE_SDK/usertools/dpdk-devbind.py" --status
    else
        warn "无法检查网卡绑定状态"
    fi
    
    echo -e "\n4. 环境变量:"
    echo "RTE_SDK: ${RTE_SDK:-未设置}"
    echo "RTE_TARGET: ${RTE_TARGET:-未设置}"
}

# 清理环境
cleanup() {
    log "开始清理DPDK环境..."
    
    # 卸载大页内存
    umount /mnt/huge 2>/dev/null || true
    
    # 恢复网卡绑定
    if [[ -n "$BIND_NICS" ]] && [[ -n "$RTE_SDK" ]]; then
        local bind_tool="$RTE_SDK/usertools/dpdk-devbind.py"
        if [[ ! -f "$bind_tool" ]]; then
            bind_tool="$RTE_SDK/tools/dpdk-devbind.py"
        fi
        
        if [[ -f "$bind_tool" ]]; then
            IFS=',' read -ra NIC_ARRAY <<< "$BIND_NICS"
            for nic in "${NIC_ARRAY[@]}"; do
                log "恢复网卡 $nic 到内核驱动..."
                python3 "$bind_tool" -b "''" "$nic" 2>/dev/null || true
            done
        fi
    fi
    
    # 移除环境变量
    rm -f /etc/profile.d/dpdk.sh
    
    log "清理完成"
}

# 显示用法
usage() {
    echo "用法: $0 [command]"
    echo "命令:"
    echo "  config     - 配置大页内存和加载驱动"
    echo "  bind       - 绑定网卡到DPDK驱动"
    echo "  status     - 显示当前状态"
    echo "  cleanup    - 清理DPDK环境"
    echo "  record-mac - record the mac of nic"
    echo "  all        - 执行所有配置步骤"
    echo ""
    echo "请先编辑 dpdk_config.conf 文件设置配置参数"
}

# 主函数
main() {
    local command="${1:-all}"
    echo $RTE_SDK
    
    case "$command" in
        record-mac)
            check_root
            load_config
            write_mac_record
            ;;
        config)
            check_root
            load_config
            configure_hugepages
            load_kernel_modules
            ;;
        bind)
            check_root
            load_config
            write_mac_record
            bind_nics
            ;;
        status)
            load_config
            show_status
            ;;
        cleanup)
            check_root
            load_config
            cleanup
            ;;
        all)
            check_root
            load_config
            write_mac_record
            configure_hugepages
            load_kernel_modules
            bind_nics
            show_status
            ;;
        *)
            usage
            exit 1
            ;;
    esac
    
    log "操作完成"
}

# 运行主函数
main "$@"
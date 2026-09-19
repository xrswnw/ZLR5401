#!/bin/bash
# ============================================================================
#  ZLR5401 工程全量快照备份脚本（遵循 /Users/swnw/Documents/Software/CLAUDE.md 规则 2）
#
#  用法:  ./backup.sh "本轮变更说明"
#  说明:
#    - 全量快照（非增量），白名单模式，排除第三方/构建/回收站。
#    - 存放于 /Volumes/Record/Person Code/BackupArea/ZLR5401/Round_<编号>_<时间戳>/
#    - 32 层轮转：超过 32 层时删除编号最小（最旧）的 Round_*。
#    - 追加日志到 .../ChangeLog/Changelog.log（单文件逐轮追加，不参与轮转）。
#    - 单层快照上限 500 MB，超限告警并列出最大子目录。
#    - 验证：源-目的文件计数对比。
# ============================================================================
set -euo pipefail

# ---------- 配置 ----------
PROJECT_NAME="ZLR5401"
SRC="/Users/swnw/Documents/Software/${PROJECT_NAME}"
BACKUP_ROOT="/Volumes/Record/Person Code/BackupArea/${PROJECT_NAME}"
GLOBAL_CLAUDE_MD="/Users/swnw/Documents/Software/CLAUDE.md"   # 全局约束文件副本
MAX_LAYERS=32
SIZE_LIMIT_MB=500

# 白名单：顶层目录（全量复制其内容）
WHITELIST_DIRS=(.claude .vscode Application Bootloader Protocol Agent ChangeLog)
# 白名单：根目录下文件（按 glob 匹配）
WHITELIST_ROOT_GLOBS=("*.sh" "CMakeLists.txt" "CLAUDE.md" ".gitignore" "toolchain-arm-none-eabi.cmake" "backup_tool.py")
# 复制时在白名单目录内部排除的条目
#   node_modules: Protocol/ 的 npm 第三方包, 按全局规则 2.3 排除第三方目录, 且曾致快照超限
#   (._* 边车无法在复制时排除, 由下方清除段统一处理)
INTERNAL_EXCLUDE=(".DS_Store" "__pycache__" ".git" "node_modules")

CHANGELOG_DIR="${BACKUP_ROOT}/ChangeLog"
CHANGELOG_FILE="${CHANGELOG_DIR}/Changelog.log"

# ---------- 参数 ----------
CHANGE_DESC="${1:-（未填写变更说明）}"

# ---------- 前置检查 ----------
if [ ! -d "${SRC}" ]; then
    echo "ERROR: 工程目录不存在: ${SRC}" >&2
    exit 1
fi
if [ ! -d "/Volumes/Record/Person Code/BackupArea" ]; then
    echo "ERROR: 备份卷未挂载: /Volumes/Record/Person Code/BackupArea" >&2
    echo "       请确认外置磁盘已接入并挂载。脚本不在本地做 fallback，遵循固定备份路径规则。" >&2
    exit 1
fi

mkdir -p "${BACKUP_ROOT}" "${CHANGELOG_DIR}"

# ---------- 确定新编号 ----------
# 收集现有 Round_* 目录的编号，取最大值 +1
LAST_NUM=0
shopt -s nullglob
for d in "${BACKUP_ROOT}"/Round_*; do
    base="$(basename "$d")"
    # Round_NNN_YYYYMMDD_HHMMSS -> 取 NNN
    num="$(echo "$base" | sed -E 's/^Round_([0-9]+)_.*/\1/')"
    # 去除前导 0 便于数值比较
    num=$((10#${num}))
    if [ "$num" -gt "$LAST_NUM" ]; then LAST_NUM="$num"; fi
done
shopt -u nullglob

NEW_NUM=$((LAST_NUM + 1))
# 编号补零到 3 位
NEW_NUM_PAD=$(printf "Round_%03d" "$NEW_NUM")
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
ROUND_DIR="${BACKUP_ROOT}/${NEW_NUM_PAD}_${TIMESTAMP}"

# 若首轮目录尚不存在任何 Round_*，CLAUDE.md 规则要求建 Round_001；
# 上面公式在空目录时已得 NEW_NUM=1，符合。
mkdir -p "${ROUND_DIR}"

echo "==> 工程备份开始"
echo "    源:        ${SRC}"
echo "    快照目录:  ${ROUND_DIR}"
echo "    变更说明:  ${CHANGE_DESC}"

COPIED=0
SKIPPED=0

# ---------- 复制白名单目录 ----------
for dir in "${WHITELIST_DIRS[@]}"; do
    src_dir="${SRC}/${dir}"
    if [ ! -e "${src_dir}" ]; then
        echo "    [skip] 白名单目录不存在: ${dir}"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi
    dst_dir="${ROUND_DIR}/${dir}"
    mkdir -p "${dst_dir}"
    # 构造 rsync 排除参数
    excl=()
    for e in "${INTERNAL_EXCLUDE[@]}"; do
        excl+=(--exclude "$e")
    done
    # 全量复制（-a 保留属性；--delete 保证快照与源一致，不留旧文件）
    rsync -a "${excl[@]}" "${src_dir}/" "${dst_dir}/"
    n=$(find "${dst_dir}" -type f | wc -l | tr -d ' ')
    COPIED=$((COPIED + n))
    echo "    [dir ] ${dir}/  -> ${n} files"
done

# ---------- 复制白名单根文件 ----------
for pat in "${WHITELIST_ROOT_GLOBS[@]}"; do
    shopt -s nullglob
    for f in "${SRC}/"$pat; do
        [ -e "$f" ] || continue
        rel="$(basename "$f")"
        cp -p "$f" "${ROUND_DIR}/${rel}"
        COPIED=$((COPIED + 1))
        echo "    [file] ${rel}"
    done
    shopt -u nullglob
done

# ---------- 全局 CLAUDE.md 副本 ----------
if [ -f "${GLOBAL_CLAUDE_MD}" ]; then
    cp -p "${GLOBAL_CLAUDE_MD}" "${ROUND_DIR}/_global_CLAUDE.md"
    COPIED=$((COPIED + 1))
    echo "    [copy] _global_CLAUDE.md (全局约束规则副本)"
fi

# ---------- exFAT ._ 边车清除 ----------
# macOS provenance 机制: 每个新写入 exFAT 卷的文件都会自动生成 ._ 边车文件
# (含 com.apple.provenance xattr, 无法从复制工具层面禁止, 即使 touch 也会生成)。
# 边车是本脚本复制过程的副产品而非工程文件, 保留会令快照文件数翻倍并在回滚时
# 污染工程目录, 故复制完成后统一清除。用户已于 2026-09-04 确认授权此清除。
APPLEDOUBLE_PURGED=$(find "${ROUND_DIR}" -name '._*' -type f -delete -print | wc -l | tr -d ' ')
if [ "${APPLEDOUBLE_PURGED}" -gt 0 ]; then
    echo "    [pur ] 已清除 ${APPLEDOUBLE_PURGED} 个 exFAT ._ 边车文件"
fi

# ---------- 快照大小统计 ----------
SNAP_SIZE_BYTES=$(du -sk "${ROUND_DIR}" | awk '{print $1 * 1024}')
SNAP_SIZE_MB=$(echo "scale=2; ${SNAP_SIZE_BYTES} / 1048576" | bc)
SIZE_STATUS="OK"
if [ "$(echo "${SNAP_SIZE_MB} > ${SIZE_LIMIT_MB}" | bc)" -eq 1 ]; then
    SIZE_STATUS="超限"
fi
echo "    快照大小:  ${SNAP_SIZE_MB}M  (${SIZE_STATUS}, 上限 ${SIZE_LIMIT_MB}M)"

# ---------- 验证：源-目的文件计数对比 ----------
# 源/目的使用同一套排除规则（与 INTERNAL_EXCLUDE 对应）：
#   .DS_Store / __pycache__ / .git / node_modules 按路径排除；._* 边车按名排除
SRC_COUNT=0
for dir in "${WHITELIST_DIRS[@]}"; do
    src_dir="${SRC}/${dir}"
    [ -e "${src_dir}" ] || continue
    n=$(find "${src_dir}" -type f \
        ! -name ".DS_Store" ! -name "._*" \
        -not -path "*/__pycache__/*" -not -path "*/.git/*" -not -path "*/node_modules/*" 2>/dev/null | wc -l | tr -d ' ')
    SRC_COUNT=$((SRC_COUNT + n))
done
# 加根文件
for pat in "${WHITELIST_ROOT_GLOBS[@]}"; do
    shopt -s nullglob
    for f in "${SRC}/"$pat; do
        [ -e "$f" ] && SRC_COUNT=$((SRC_COUNT + 1))
    done
    shopt -u nullglob
done
[ -f "${GLOBAL_CLAUDE_MD}" ] && SRC_COUNT=$((SRC_COUNT + 1))

DST_COUNT=$(find "${ROUND_DIR}" -type f ! -name "._*" | wc -l | tr -d ' ')
VERIFY="源=${SRC_COUNT} 目的=${DST_COUNT}"
if [ "${SRC_COUNT}" -ne "${DST_COUNT}" ]; then
    VERIFY="${VERIFY} [WARN: 计数不一致]"
    echo "    [WARN] 文件计数不一致: 源=${SRC_COUNT} 目的=${DST_COUNT}"
else
    echo "    [ok  ] 文件计数一致: ${SRC_COUNT}"
fi

# ---------- 32 层轮转 ----------
ROTATE_ACTION="无"
shopt -s nullglob
ROUND_DIRS=("${BACKUP_ROOT}"/Round_*)
shopt -u nullglob
ROUND_COUNT=${#ROUND_DIRS[@]}
if [ "$ROUND_COUNT" -gt "$MAX_LAYERS" ]; then
    # 找编号最小的 Round_* 删除（含当前刚建的，但当前是最大编号，不会被删）
    OLDEST=""
    OLDEST_NUM=999999
    for d in "${ROUND_DIRS[@]}"; do
        base="$(basename "$d")"
        num="$(echo "$base" | sed -E 's/^Round_([0-9]+)_.*/\1/')"
        num=$((10#${num}))
        if [ "$num" -lt "$OLDEST_NUM" ]; then
            OLDEST_NUM=$num
            OLDEST="$d"
        fi
    done
    if [ -n "$OLDEST" ]; then
        # 规则 2.2 明确授权删除最旧 Round_*（备份轮转，非工程源码删除）
        rm -rf "${OLDEST}"
        ROTATE_ACTION="删除 $(basename "$OLDEST")"
        echo "    [rot ] 超过 ${MAX_LAYERS} 层，已删除最旧: $(basename "$OLDEST")"
    fi
fi

# ---------- 超限告警：列出最大子目录 ----------
if [ "$SIZE_STATUS" = "超限" ]; then
    echo "    [WARN] 快照超过 ${SIZE_LIMIT_MB}M，最大子目录:"
    du -sh "${ROUND_DIR}"/*/ 2>/dev/null | sort -rh | head -5 | sed 's/^/        /'
fi

# ---------- 写变更日志 ----------
NOW="$(date '+%Y-%m-%d %H:%M:%S')"
cat >> "${CHANGELOG_FILE}" <<EOF
[${NOW}] Round ${NEW_NUM}
变更说明: ${CHANGE_DESC}
备份范围: 白名单目录(${WHITELIST_DIRS[*]}) + 根脚本/文件(${WHITELIST_ROOT_GLOBS[*]}) + 全局CLAUDE.md副本
快照路径: ${ROUND_DIR}
快照大小: ${SNAP_SIZE_MB}M  (<=${SIZE_LIMIT_MB}M OK / 超限标记: ${SIZE_STATUS})
轮转动作: ${ROTATE_ACTION}
验证: 复制项=${COPIED} 跳过=${SKIPPED} / 源-目的文件计数对比 ${VERIFY}
---
EOF

echo "==> 备份完成"
echo "    日志: ${CHANGELOG_FILE}"
echo "    快照: ${ROUND_DIR}  (${SNAP_SIZE_MB}M)"

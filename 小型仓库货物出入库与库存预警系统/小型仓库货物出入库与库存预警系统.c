/* 数组保存货物、批次及逐笔出入库流水；先到期先出库。
 * 日期范围2000至2099年，到期当天报废但保留数量；数据自动保存到程序目录warehouse.dat。 */
#define _CRT_SECURE_NO_WARNINGS /* 允许在Visual Studio中使用标准C输入输出函数。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <windows.h>
#include <wchar.h>
#define MAX_GOODS 100 /* 货物种类数组容量上限。 */
#define MAX_BATCHES 1000 /* 批次数组容量上限。 */
#define MAX_RECORDS 146100 /* 逐笔操作明细数组容量上限。 */
#define LAST_DAY 36524 /* 2099-12-31对应的日序数；基准日为2000-01-01。 */
#define DATA_VERSION 1 /* 当前数据文件格式版本号，与日期无关。 */
#define LEGACY_DATA_VERSION 20261106 /* 兼容之前保存的逐笔明细文件。 */
enum { STOCK_IN, STOCK_OUT, RETURN_IN, SCRAP }; /* 流水类型依次为入库、出库、退货、报废。 */
typedef struct { int id; char name[50]; int minStock; } Goods; /* Goods字段：id为唯一货物编号，name为名称（最多49字节），minStock为安全库存阈值。 */
typedef struct { int id, goodsIndex, entryDate, shelfLife, quantity, scrapped; } Batch; /* Batch字段：id为唯一批次编号，goodsIndex为所属货物下标，entryDate为入库日序数，shelfLife为保质天数，quantity为剩余/过期保留数量；scrapped为0未计报废、1已计报废、2旧版数量未知。 */
typedef struct { int date, type, batchId, quantity; } StockRecord; /* StockRecord字段：date为操作日序数，type为流水类型，batchId为关联批次编号，quantity为本次操作数量。 */
Goods goods[MAX_GOODS]; /* goods保存每种货物的基本资料。 */
Batch batches[MAX_BATCHES]; /* batches保存各批次日期、数量及报废状态。 */
StockRecord records[MAX_RECORDS]; /* records保存可用于周期统计的逐笔操作记录。 */
int goodsCount, batchCount, recordCount, currentDate; /* 前三个变量为各数组有效元素数；currentDate为当前日期日序数。 */
wchar_t dataPath[32768], tempPath[32768]; /* dataPath为正式数据文件路径，tempPath为保存时使用的临时文件路径。 */
int readLine(char *s, int size) /* 读取一行并去除首尾空白；s为接收数组，size为容量；返回1成功、0输入结束、-1过长。 */
{
    int ch; /* ch暂存字符，用于清除过长输入的剩余内容。 */
    size_t n, start = 0; /* n为当前字符串长度，start为第一个非空白字符的位置。 */
    if (!fgets(s, size, stdin)) return 0;
    n = strlen(s);
    if (n && s[n - 1] == '\n') s[--n] = '\0';
    else if ((ch = getchar()) != '\n' && ch != EOF) {
        while ((ch = getchar()) != '\n' && ch != EOF) { }
        puts("输入过长。");
        return -1;
    }
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    while (start < n && isspace((unsigned char)s[start])) start++;
    memmove(s, s + start, n - start + 1);
    return 1;
}
int readInt(const char *prompt, int low, int high, int *value) /* 读取合法整数；prompt为提示，low/high为范围，value接收结果；返回1成功、0输入结束。 */
{
    char s[256], *end; /* s保存输入文本，end指向strtol转换停止的位置。 */
    long n; /* n暂存转换结果，通过范围检查后再赋给int变量。 */
    int result; /* result保存readLine返回的成功、结束或过长状态。 */
    for (;;) {
        printf("%s", prompt); fflush(stdout);
        result = readLine(s, sizeof(s));
        if (!result) return 0;
        if (result < 0) continue;
        errno = 0;
        n = strtol(s, &end, 10);
        if (end != s && !*end && errno != ERANGE && n >= low && n <= high) {
            *value = (int)n;
            return 1;
        }
        printf("请输入%d至%d的整数。\n", low, high);
    }
}
int monthDays(int y, int m) /* 根据年份y、月份m计算当月天数并返回，包含闰年判断。 */
{
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31}; /* days按1至12月保存平年天数；闰年2月另加1天。 */
    return days[m - 1] + (m == 2 && (y % 400 == 0 || (y % 4 == 0 && y % 100 != 0)));
}
int dateToDay(int y, int m, int d) /* 将合法年月日y/m/d转换为日序数返回；2000-01-01为第0天。 */
{
    int i, n = d - 1; /* i用于遍历先前年月，n累计从基准日到目标日期的天数。 */
    for (i = 2000; i < y; i++) n += 337 + monthDays(i, 2);
    for (i = 1; i < m; i++) n += monthDays(y, i);
    return n;
}
void formatDate(int n, char *s) /* 将日序数n转换为YYYY-MM-DD并写入数组s；s至少11字节，无返回值。 */
{
    int y = 2000, m = 1; /* y/m为逐步推算的年、月；参数n逐步扣除完整年月后为月内偏移。 */
    while (n >= 337 + monthDays(y, 2)) n -= 337 + monthDays(y++, 2);
    while (n >= monthDays(y, m)) { n -= monthDays(y, m); m++; }
    sprintf(s, "%04d-%02d-%02d", y, m, n + 1);
}
int readDate(const char *prompt, int *date) /* prompt为提示，date接收日期日序数；检查真实日期，返回1成功、0输入结束。 */
{
    int n, y, m, d; /* n为输入的八位日期整数，y/m/d为拆分出的年、月、日。 */
    while (readInt(prompt, 20000101, 20991231, &n)) {
        y = n / 10000; m = n / 100 % 100; d = n % 100;
        if (m >= 1 && m <= 12 && d >= 1 && d <= monthDays(y, m)) {
            *date = dateToDay(y, m, d);
            return 1;
        }
        puts("日期无效。");
    }
    return 0;
}
int findGoods(int id) /* 按货物编号id顺序查找；返回goods下标，未找到返回-1。 */
{
    int i; /* i遍历货物数组下标。 */
    for (i = 0; i < goodsCount; i++) if (goods[i].id == id) return i;
    return -1;
}
int findBatch(int id) /* 按批次编号id顺序查找；返回batches下标，未找到返回-1。 */
{
    int i; /* i遍历批次数组下标。 */
    for (i = 0; i < batchCount; i++) if (batches[i].id == id) return i;
    return -1;
}
long long totalStock(int index) /* index为货物下标；累加其所有未过期批次数量，返回可用库存。 */
{
    int i; /* i遍历每个批次，筛选指定货物且未过期的批次。 */
    long long total = 0; /* total累计可用库存，使用长整数避免多个批次相加溢出int。 */
    for (i = 0; i < batchCount; i++)
        if (batches[i].goodsIndex == index &&
            batches[i].entryDate + batches[i].shelfLife > currentDate) total += batches[i].quantity;
    return total;
}
void addRecord(int date, int type, int batchId, int quantity) /* date/type/batchId/quantity为日期、类型、批次编号、数量；追加一条明细，无返回值。 */
{
    StockRecord r = {date, type, batchId, quantity}; /* r为本次待追加的操作记录。 */
    if (recordCount == MAX_RECORDS) {
        puts("记录已满，操作未保存，程序停止。");
        exit(1);
    }
    records[recordCount++] = r; /* 修改库存前已预留足够的记录空间。 */
}
int loadData(void) /* 无参数；从文件读入并校验全部数据，返回1成功（含首次无文件）、0失败；不写文件。 */
{
    FILE *fp = _wfopen(dataPath, L"rb"); /* fp为二进制只读文件指针；打开失败时为NULL。 */
    int h[4], ok, i; /* h依次保存版本及货物/批次/明细数量；ok表示校验是否通过，i为遍历下标。 */
    if (!fp) return errno == ENOENT;
    ok = fread(h, sizeof(h), 1, fp) == 1 && (h[0] == DATA_VERSION || h[0] == LEGACY_DATA_VERSION) &&
        h[1] >= 0 && h[1] <= MAX_GOODS && h[2] >= 0 && h[2] <= MAX_BATCHES &&
        h[3] >= 0 && h[3] <= MAX_RECORDS;
    if (ok) {
        goodsCount = h[1];
        batchCount = h[2];
        recordCount = h[3];
        ok = fread(goods, sizeof(Goods), goodsCount, fp) == (size_t)goodsCount;
        ok = ok && fread(batches, sizeof(Batch), batchCount, fp) == (size_t)batchCount &&
            fread(records, sizeof(StockRecord), recordCount, fp) == (size_t)recordCount &&
            fgetc(fp) == EOF && !ferror(fp);
    }
    fclose(fp);
    for (i = 0; ok && i < goodsCount; i++)
        ok = goods[i].id > 0 && goods[i].minStock >= 0 && goods[i].name[0] &&
            memchr(goods[i].name, 0, sizeof(goods[i].name)) && findGoods(goods[i].id) == i;
    for (i = 0; ok && i < batchCount; i++)
        ok = batches[i].id > 0 && findBatch(batches[i].id) == i &&
            batches[i].goodsIndex >= 0 && batches[i].goodsIndex < goodsCount &&
            batches[i].entryDate >= 0 && batches[i].shelfLife > 0 &&
            batches[i].shelfLife <= LAST_DAY &&
            batches[i].entryDate <= LAST_DAY - batches[i].shelfLife && batches[i].quantity >= 0 &&
            batches[i].scrapped >= 0 && batches[i].scrapped <= 2 &&
            (batches[i].scrapped != 2 || batches[i].quantity == 0);
    for (i = 0; ok && i < recordCount; i++)
        ok = records[i].date >= 0 && records[i].date <= LAST_DAY && records[i].quantity > 0 &&
            records[i].type >= STOCK_IN && records[i].type <= SCRAP && findBatch(records[i].batchId) >= 0;
    return ok;
}
int saveData(void) /* 无参数；先写临时文件再替换正式文件，返回1保存成功、0失败。 */
{
    int h[] = {DATA_VERSION, goodsCount, batchCount, recordCount}, ok; /* h为待写入的文件头；ok表示写入、关闭和替换是否成功。 */
    FILE *fp = _wfopen(tempPath, L"wb"); /* fp为临时数据文件的二进制写入指针。 */
    if (!fp) return 0;
    ok = fwrite(h, sizeof(h), 1, fp) == 1 &&
        fwrite(goods, sizeof(Goods), goodsCount, fp) == (size_t)goodsCount &&
        fwrite(batches, sizeof(Batch), batchCount, fp) == (size_t)batchCount &&
        fwrite(records, sizeof(StockRecord), recordCount, fp) == (size_t)recordCount;
    if (fclose(fp) != 0) ok = 0;
    if (ok) ok = MoveFileExW(tempPath, dataPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) _wremove(tempPath);
    return ok;
}
void scrapExpired(void) /* 无参数、无返回值；为到期批次记录一次报废，保留数量并标记，避免重复统计。 */
{
    int i; /* i遍历批次数组。 */
    long long total = 0; /* total累计本次新发现的过期数量，用于打印提示。 */
    for (i = 0; i < batchCount; i++)
        if (!batches[i].scrapped && batches[i].entryDate + batches[i].shelfLife <= currentDate) {
            if (batches[i].quantity)
                addRecord(batches[i].entryDate + batches[i].shelfLife, SCRAP, batches[i].id, batches[i].quantity);
            total += batches[i].quantity;
            batches[i].scrapped = 1;
        }
    if (total) printf("自动报废%lld件。\n", total);
}
void printName(const char *name) /* name为UTF-8名称；按中文占两列输出并补空格，无返回值。 */
{
    int width = 0, i, padding = 12; /* width为名称显示宽度，i遍历货物，padding为统一列宽（至少12列）。 */
    const unsigned char *p; /* p逐字节扫描UTF-8名称，区分ASCII与多字节字符。 */
    for (p = (const unsigned char *)name; *p; p++)
        if (*p < 128) width++;
        else if ((*p & 0xC0) != 0x80) width += 2;
    for (i = 0; i < goodsCount; i++)
        if ((int)strlen(goods[i].name) > padding) padding = (int)strlen(goods[i].name);
    printf("%s", name);
    while (width++ < padding) putchar(' ');
}
int warnGoods(int i) /* i为货物下标；低于安全阈值时打印预警并返回1，否则返回0。 */
{
    long long n = totalStock(i); /* n为该货物未过期的可用库存。 */
    if (n >= goods[i].minStock) return 0;
    printf("预警 %10d | ", goods[i].id);
    printName(goods[i].name);
    printf(" | 库存 %13lld | 预警值 %10d\n", n, goods[i].minStock);
    return 1;
}
void showBatch(int i) /* i为批次下标；输出批次日期、数量和过期状态，无返回值。 */
{
    char entry[11], expiry[11]; /* entry/expiry分别保存入库日期和到期日期文本，含字符串结束符。 */
    formatDate(batches[i].entryDate, entry);
    formatDate(batches[i].entryDate + batches[i].shelfLife, expiry);
    printf("  批次 %10d | 入库 %s | 保质期 %5d天 | 到期 %s | 数量 ",
        batches[i].id, entry, batches[i].shelfLife, expiry);
    if (batches[i].scrapped == 2) printf("      未知");
    else printf("%10d", batches[i].quantity);
    puts(batches[i].entryDate + batches[i].shelfLife <= currentDate ? " | 已过期（不计入库存）" : " | 正常");
}
void receiveBatch(int isReturn) /* isReturn为0表示普通入库、1表示退货；校验完成后新增批次及明细，无返回值。 */
{
    Goods item = {0}; /* item暂存新货物信息，输入完整且有效后才加入数组。 */
    Batch b = {0}; /* b暂存新批次，初始报废标记为0。 */
    char name[256]; /* name暂存输入名称，校验长度后再复制到item.name。 */
    int result, isNew; /* result为读行状态，isNew表示该货物编号是否尚未存在。 */
    if (batchCount == MAX_BATCHES) { puts("批次已满。"); return; }
    if (!readInt("货物编号：", 1, INT_MAX, &item.id)) return;
    b.goodsIndex = findGoods(item.id); isNew = b.goodsIndex == -1;
    if (isNew) {
        if (goodsCount == MAX_GOODS) { puts("货物种类已满。"); return; }
        for (;;) {
            printf("名称："); fflush(stdout);
            result = readLine(name, sizeof(name));
            if (!result) return;
            if (result > 0 && *name && strlen(name) < sizeof(item.name)) break;
            puts("名称为空或过长。");
        }
        strcpy(item.name, name);
        if (!readInt("预警值：", 0, INT_MAX, &item.minStock)) return;
        b.goodsIndex = goodsCount;
    }
    if (!readInt("批次编号：", 1, INT_MAX, &b.id)) return;
    if (findBatch(b.id) != -1) { puts("批次编号重复。"); return; }
    if (!readInt("数量：", 1, INT_MAX, &b.quantity)) return;
    do {
        if (!readDate("入库/退货日期（YYYYMMDD）：", &b.entryDate)) return;
        if (b.entryDate <= currentDate) break;
        puts("日期不能晚于今天。");
    } while (1);
    if (b.entryDate == LAST_DAY) { puts("日期超出计算范围。"); return; }
    if (!readInt(isReturn ? "剩余保质期（天）：" : "保质期（天）：",
        1, LAST_DAY - b.entryDate, &b.shelfLife)) return;
    if (isNew) goods[goodsCount++] = item;
    batches[batchCount++] = b;
    addRecord(b.entryDate, isReturn ? RETURN_IN : STOCK_IN, b.id, b.quantity);
    puts("入库成功。"); showBatch(batchCount - 1);
}
void stockOut(void) /* 无参数、无返回值；读取编号和数量，先检查总库存，再按先到期顺序逐批扣减。 */
{
    int id, index, n, left, best, i, taken; /* id为货物编号，index为货物下标，n为请求数量，left为待出数量，best为最早到期批次下标（-1未找到），i为遍历下标，taken为本批扣减数量。 */
    if (!readInt("货物编号：", 1, INT_MAX, &id)) return;
    index = findGoods(id);
    if (index < 0) { puts("货物不存在。"); return; }
    if (!readInt("出库数量：", 1, INT_MAX, &n)) return;
    if (totalStock(index) < n) { puts("库存不足，出库取消。"); return; }
    for (left = n; left > 0; left -= taken) {
        best = -1;
        for (i = 0; i < batchCount; i++)
            if (batches[i].goodsIndex == index && batches[i].quantity > 0 &&
                batches[i].entryDate + batches[i].shelfLife > currentDate &&
                (best < 0 || batches[i].entryDate + batches[i].shelfLife <
                    batches[best].entryDate + batches[best].shelfLife)) best = i;
        if (best < 0 || best >= batchCount || best >= MAX_BATCHES) // 检查下标后才能读取批次。
            return;
        taken = left < batches[best].quantity ? left : batches[best].quantity;
        addRecord(currentDate, STOCK_OUT, batches[best].id, taken);
        batches[best].quantity -= taken;
        printf("批次%d出库%d，剩余%d。\n", batches[best].id, taken, batches[best].quantity);
    }
    puts("出库成功。");
}
void showGoods(int i) /* i为货物下标；输出该货物可用库存及全部批次（含零库存、过期批次），无返回值。 */
{
    int j; /* j遍历该货物所属的全部批次。 */
    printf("货物 %10d | ", goods[i].id);
    printName(goods[i].name);
    printf(" | 库存 %13lld | 预警值 %10d\n", totalStock(i), goods[i].minStock);
    for (j = 0; j < batchCount; j++)
        if (batches[j].goodsIndex == i) showBatch(j);
}
void queryGoods(int all) /* all为1查询全部、0读取编号查询单种；顺序查找并显示结果，无返回值。 */
{
    int id = 0, i, count = 0; /* id为待查编号，i遍历货物数组，count为找到的货物种类数。 */
    long long total = 0; /* total累计查询结果的可用库存，供全部库存合计使用。 */
    if (!all && !readInt("货物编号：", 1, INT_MAX, &id)) return;
    if (all) puts("\n---------------------- 全部库存 ----------------------");
    for (i = 0; i < goodsCount; i++)
        if (all || goods[i].id == id) {
            showGoods(i);
            total += totalStock(i);
            count++;
        }
    if (!count) puts(all ? "仓库暂无货物。" : "货物不存在。");
    if (all) printf("合计：%d种货物，%d个批次，库存%lld件。\n", goodsCount, batchCount, total);
}
void checkExpiring(void) /* 无参数、无返回值；读取临期天数，列出该范围内未过期且有数量的批次。 */
{
    int days, i, count = 0; /* days为临期天数，i为批次下标，count统计符合条件的批次数。 */
    if (!readInt("临期天数：", 0, LAST_DAY - currentDate, &days)) return;
    for (i = 0; i < batchCount; i++)
        if (batches[i].quantity && batches[i].entryDate + batches[i].shelfLife > currentDate &&
            batches[i].entryDate + batches[i].shelfLife <= currentDate + days) {
            printf("%d %s\n", goods[batches[i].goodsIndex].id, goods[batches[i].goodsIndex].name);
            showBatch(i); count++;
        }
    if (!count) puts("没有临期货物。");
}
void statistics(void) /* 无参数、无返回值；读取起止日期（含两端），分组显示明细，最后输出各类总量。 */
{
    int begin, end, i, b, group; /* begin/end为起止日序数，i为明细下标，b为对应批次下标，group为当前分组（0入库含退货、1出库、2报废）。 */
    char date[11]; /* date保存当前明细的操作日期文本。 */
    const char *titles[] = {"入库明细（含退货）", "出库明细", "过期报废明细（日期为到期日）"}; /* titles保存三个统计分组的显示标题。 */
    long long totals[4] = {0}; /* totals按流水类型累计入库、出库、退货、报废数量。 */
    if (!readDate("开始日期（YYYYMMDD）：", &begin) || !readDate("结束日期（YYYYMMDD）：", &end)) return;
    if (begin > end) { puts("日期顺序错误。"); return; }
    for (group = 0; group < 3; group++) {
        printf("\n-------------------- %s --------------------\n", titles[group]);
        printf("日期       | 商品编号 | ");
        printName("商品名称");
        puts(" | 批次编号   | 数量       | 备注");
        for (i = 0; i < recordCount; i++)
            if (records[i].date >= begin && records[i].date <= end &&
                (records[i].type == RETURN_IN ? 0 : records[i].type == SCRAP ? 2 : records[i].type) == group) {
                b = findBatch(records[i].batchId);
                formatDate(records[i].date, date);
                printf("%s | %8d | ", date, goods[batches[b].goodsIndex].id);
                printName(goods[batches[b].goodsIndex].name);
                printf(" | %10d | %10d | %s\n", records[i].batchId, records[i].quantity,
                    records[i].type == RETURN_IN ? "退货入库" : "");
                totals[records[i].type] += records[i].quantity;
            }
    }
    puts("\n-------------------- 周期总量 --------------------");
    printf("入库总量：%10lld（其中退货：%lld）\n出库总量：%10lld\n过期报废：%10lld\n",
        totals[STOCK_IN] + totals[RETURN_IN], totals[RETURN_IN], totals[STOCK_OUT], totals[SCRAP]);
}
void takeInventory(void) /* 无参数、无返回值；读取批次和实盘数量，修正未过期库存，不记正常出入库流水。 */
{
    int id, i, n; /* id为批次编号，i为找到的批次下标，n为输入的实盘数量。 */
    if (!readInt("批次编号：", 1, INT_MAX, &id)) return;
    i = findBatch(id);
    if (i < 0) { puts("批次不存在。"); return; }
    showBatch(i);
    if (batches[i].entryDate + batches[i].shelfLife <= currentDate) {
        puts("该批次已过期，保留过期数量，不能修改可用库存。");
        return;
    }
    if (!readInt("实盘数量：", 0, INT_MAX, &n)) return;
    printf("盘点差额：%d\n", n - batches[i].quantity);
    batches[i].quantity = n;
}
void deleteData(void) /* 无参数、无返回值；确认后删除指定商品或批次，并同步移除对应明细、修正数组下标。 */
{
    int mode, id, index, yes, i, j, count = 0; /* mode为删除类型，id为目标编号，index为目标下标，yes为确认选择，i为遍历下标，j为明细对应批次下标，count为压缩后保留的数据数量。 */
    puts("\n---------------------- 删除管理 ----------------------");
    puts("1. 删除商品及全部批次    2. 删除某个批次    0. 返回");
    if (!readInt("请选择删除类型：", 0, 2, &mode) || !mode) return;
    if (!readInt(mode == 1 ? "商品编号：" : "批次编号：", 1, INT_MAX, &id)) return;
    index = mode == 1 ? findGoods(id) : findBatch(id);
    if (index < 0) {
        puts(mode == 1 ? "商品不存在。" : "批次不存在。");
        return;
    }
    if (mode == 1) showGoods(index);
    else {
        printf("所属商品：%d %s\n", goods[batches[index].goodsIndex].id,
            goods[batches[index].goodsIndex].name);
        showBatch(index);
    }
    puts("删除后移除上述库存资料及其全部出入库、报废统计记录。");
    if (!readInt("确认删除？（1确认/0取消）：", 0, 1, &yes) || !yes) {
        puts("已取消删除。");
        return;
    }
    /* 批次还在时先移除其明细，避免遗留无对应批次的统计。 */
    for (i = 0; i < recordCount; i++) {
        j = findBatch(records[i].batchId);
        if ((mode == 1 && batches[j].goodsIndex == index) || (mode == 2 && j == index)) continue;
        records[count++] = records[i];
    }
    recordCount = count;
    count = 0;
    /* 数组压缩：跳过待删除批次，保留其余批次的原有顺序。 */
    for (i = 0; i < batchCount; i++) {
        if ((mode == 1 && batches[i].goodsIndex == index) || (mode == 2 && i == index)) continue;
        batches[count] = batches[i];
        if (mode == 1 && batches[count].goodsIndex > index) batches[count].goodsIndex--;
        count++;
    }
    batchCount = count;
    if (mode == 1) {
        /* 商品数组前移后，同步修正其他批次所引用的商品下标。 */
        for (i = index; i < goodsCount - 1; i++) goods[i] = goods[i + 1];
        goodsCount--;
    }
    puts(mode == 1 ? "商品及其全部批次已删除。" : "批次已删除，所属商品保留。");
}
void showMenu(void) /* 无参数、无返回值；显示主菜单、当前日期及货物和批次数量。 */
{
    char date[11]; /* date保存菜单显示的当前日期文本。 */
    formatDate(currentDate, date);
    puts("\n+--------------------------------------------------+");
    puts("|       小型仓库货物出入库与库存预警系统           |");
    puts("+--------------------------------------------------+");
    puts("|              1. 货物入库                         |");
    puts("|              2. 货物出库                         |");
    puts("|              3. 退货入库                         |");
    puts("|              4. 货物批次查询                     |");
    puts("|              5. 即将过期查询                     |");
    puts("|              6. 库存预警清单                     |");
    puts("|              7. 周期统计                         |");
    puts("|              8. 库存盘点                         |");
    puts("|              9. 查询全部库存                     |");
    puts("|             10. 删除管理                         |");
    puts("+--------------------------------------------------+");
    puts("|              0. 保存并退出                       |");
    puts("+--------------------------------------------------+");
    printf("  日期：%s  |  货物：%d种  |  批次：%d批\n",
        date, goodsCount, batchCount);
}
int initSystem(void) /* 无参数；设置数据路径和中文控制台，加载库存；返回1成功、0初始化失败。 */
{
    DWORD length = GetModuleFileNameW(NULL, dataPath, 32768); /* length为程序路径长度，失败时为0。 */
    wchar_t *slash = wcsrchr(dataPath, L'\\'); /* slash为最后一个路径分隔符的位置。 */
    if (!length || length >= 32740 || !slash) return 0;
    wcscpy(slash + 1, L"warehouse.dat");
    wcscpy(tempPath, dataPath);
    wcscat(tempPath, L".tmp");
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    return loadData();
}
int updateDate(void) /* 无参数；更新当前日期日序数；返回1成功、0系统日期超范围或转换失败。 */
{
    time_t now = time(NULL); /* now为系统当前时间。 */
    struct tm *today = localtime(&now); /* today指向转换后的本地年月日。 */
    if (!today || today->tm_year < 100 || today->tm_year > 199) return 0;
    currentDate = dateToDay(today->tm_year + 1900, today->tm_mon + 1, today->tm_mday);
    return 1;
}
void handleChoice(int choice) /* choice为菜单编号；执行功能、检查报废并显示库存预警，无返回值。 */
{
    int i, count; /* i为货物下标，count为触发预警的货物种类数。 */
    /* 为跨批次出库和未来自动报废预留记录空间，满时仍可查询或删除。 */
    if (choice >= 1 && choice <= 3 && recordCount + 2 * batchCount + 3 > MAX_RECORDS) {
        puts("记录容量不足，请先删除不需要的商品或批次。");
        return;
    }
    switch (choice) {
    case 1: receiveBatch(0); break;
    case 2: stockOut(); break;
    case 3: receiveBatch(1); break;
    case 4: queryGoods(0); break;
    case 5: checkExpiring(); break;
    case 6:
        for (i = count = 0; i < goodsCount; i++) count += warnGoods(i);
        if (!count) puts("没有库存预警。");
        break;
    case 7: statistics(); break;
    case 8: takeInventory(); break;
    case 9: queryGoods(1); break;
    case 10: deleteData(); break;
    }
    scrapExpired();
    if (choice == 1 || choice == 2 || choice == 3 || choice == 8 || choice == 10)
        for (i = 0; i < goodsCount; i++) warnGoods(i);
}
int main(void) /* 程序入口；初始化后循环显示菜单、执行功能和保存；返回0正常退出、1运行失败。 */
{
    int choice; /* choice保存用户选择的功能编号。 */
    if (!initSystem()) {
        puts("初始化失败，请检查数据文件格式、目录或读取权限，原文件未修改。");
        return 1;
    }
    for (;;) {
        if (!updateDate()) {
            puts("系统日期应在2000至2099年之间。");
            return 1;
        }
        scrapExpired();
        if (!saveData()) {
            puts("保存失败，请检查目录权限或磁盘空间，程序停止。");
            return 1;
        }
        showMenu();
        if (!readInt("请选择功能（0-10）：", 0, 10, &choice) || !choice) break;
        handleChoice(choice); /* 下一轮显示菜单前自动保存本次操作结果。 */
    }
    puts("程序正常退出。");
    return 0;
}

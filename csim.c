// student id: 2025201765
// please change the above line to your student id

#include <assert.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// 定义缓存行的数据结构
typedef struct
{
  int valid;           // 有效位：判断缓存行是否有效
  int dirty;           // 脏位：记录数据是否被修改
  uint64_t tag;        // 标记位：判断是否为需要的数据块
  uint64_t last_used;  // 最近访问时间，用于 LRU 替换
} CacheLine;

void printSummary(int hits, int misses, int evictions)
{
  printf("hits:%d misses:%d evictions:%d\n", hits, misses, evictions);
  FILE *output_fp = fopen(".csim_results", "w");
  assert(output_fp);
  fprintf(output_fp, "%d %d %d\n", hits, misses, evictions);
  fclose(output_fp);
}

void printHelp(const char *name)
{
  printf(
      "Usage: %s [-hv] -s <num> -E <num> -b <num> -t <file>\n"
      "Options:\n"
      "  -h         Print this help message.\n"
      "  -v         Optional verbose flag.\n"
      "  -s <num>   Number of set index bits.\n"
      "  -E <num>   Number of lines per set.\n"
      "  -b <num>   Number of block offset bits.\n"
      "  -t <file>  Trace file.\n\n"
      "Examples:\n"
      "  linux>  %s -s 4 -E 1 -b 4 -t traces/yi.trace\n"
      "  linux>  %s -v -s 8 -E 2 -b 4 -t traces/yi.trace\n",
      name, name, name);
}

// 模拟一次内存读取或写入
static void accessCache(CacheLine *cache, int s, int E, int b,
                        uint64_t address, int is_store, uint64_t *clock,
                        int *hits, int *misses, int *evictions, int verbose)
{
  // 根据内存地址计算缓存组索引和标记
  uint64_t block_address = address >> b;
  size_t set_index = (size_t)(block_address & ((UINT64_C(1) << s) - 1));
  uint64_t tag = block_address >> s;

  // 找到对应的缓存组
  CacheLine *set = cache + set_index * (size_t)E;

  int empty = -1;
  int lru = 0;

  // 更新时间戳
  ++(*clock);

  // 遍历当前缓存组的所有缓存行
  for (int i = 0; i < E; ++i)
  {
    // 判断是否命中
    if (set[i].valid && set[i].tag == tag)
    {
      ++(*hits);

      // 更新最近访问时间
      set[i].last_used = *clock;

      // 如果是写入操作，则标记为脏
      if (is_store)
        set[i].dirty = 1;

      if (verbose)
        printf(" hit");

      return;
    }

    // 寻找空闲缓存行
    if (!set[i].valid && empty == -1)
      empty = i;

    // 寻找最久未使用的缓存行
    if (set[i].valid && set[i].last_used < set[lru].last_used)
      lru = i;
  }

  // 没有找到匹配的缓存行，发生 Miss
  ++(*misses);

  if (verbose)
    printf(" miss");

  // 优先使用空闲缓存行
  int victim = empty;

  // 如果没有空闲缓存行，就执行 LRU 替换
  if (victim == -1)
  {
    ++(*evictions);

    victim = lru;

    if (verbose)
      printf(" eviction");

    // Write-Back 策略：
    // 如果被替换的缓存行是脏的，需要将其写回主存。
    // 本实验的计数器不额外统计该写回操作。
  }

  // 将新的内存块放入缓存行
  set[victim].valid = 1;
  set[victim].tag = tag;
  set[victim].dirty = is_store;
  set[victim].last_used = *clock;
}

int main(int argc, char *argv[])
{
  // Cache 参数
  int s = -1;
  int E = -1;
  int b = -1;

  // Trace 文件路径
  char *trace_file = NULL;

  // 是否开启详细输出
  int verbose = 0;

  int opt;

  // 解析命令行参数
  while ((opt = getopt(argc, argv, "hvs:E:b:t:")) != -1)
  {
    switch (opt)
    {
    case 'h':
      printHelp(argv[0]);
      return 0;

    case 'v':
      verbose = 1;
      break;

    case 's':
      s = atoi(optarg);
      break;

    case 'E':
      E = atoi(optarg);
      break;

    case 'b':
      b = atoi(optarg);
      break;

    case 't':
      trace_file = optarg;
      break;

    default:
      printHelp(argv[0]);
      return 1;
    }
  }

  // 检查输入参数是否合法
  if (s < 0 || E <= 0 || b < 0 || s >= 63 || b >= 64 || s + b >= 64 ||
      trace_file == NULL)
  {
    fprintf(stderr, "Error: Invalid or missing arguments.\n");
    printHelp(argv[0]);
    return 1;
  }

  // 计算缓存组数
  size_t set_count = (size_t)1 << s;

  // 检查缓存大小是否过大
  if ((size_t)E > SIZE_MAX / sizeof(CacheLine) / set_count)
  {
    fprintf(stderr, "Error: Cache size is too large.\n");
    return 1;
  }

  // 动态申请缓存空间
  CacheLine *cache = calloc(set_count * (size_t)E, sizeof(CacheLine));

  if (cache == NULL)
  {
    perror("calloc");
    return 1;
  }

  // 打开 Trace 文件
  FILE *fp = fopen(trace_file, "r");

  if (fp == NULL)
  {
    perror(trace_file);
    free(cache);
    return 1;
  }

  // 初始化统计变量
  int hits = 0;
  int misses = 0;
  int evictions = 0;

  // LRU 时间戳
  uint64_t clock = 0;

  // 保存读取到的一行数据
  char line[256];

  // 逐行读取 Trace 文件
  while (fgets(line, sizeof(line), fp) != NULL)
  {
    char operation;
    unsigned long long address;
    unsigned int size;

    // 解析操作类型、地址和访问大小
    if (sscanf(line, " %c %llx,%u", &operation, &address, &size) != 3)
      continue;

    // 只处理读取 L 和写入 S
    if (operation != 'L' && operation != 'S')
      continue;

    // 详细输出
    if (verbose)
      printf("%c %llx,%u", operation, address, size);

    // 模拟缓存访问
    accessCache(cache, s, E, b, (uint64_t)address, operation == 'S', &clock,
                &hits, &misses, &evictions, verbose);

    if (verbose)
      putchar('\n');
  }

  // 关闭文件
  fclose(fp);

  // 释放动态申请的内存
  free(cache);

  // 输出最终统计结果
  printSummary(hits, misses, evictions);

  return 0;
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <numeric>
#include <thread>
#include <random>
#include <queue>

#include "DynamicKomi.h"
#include "GoBoard.h"
#include "Ladder.h"
#include "Message.h"
#include "MoveCache.h"
#include "PatternHash.h"
#include "Point.h"
#include "Rating.h"
#include "Seki.h"
#include "Simulation.h"
#include "UctRating.h"
#include "UctSearch.h"
#include "Utility.h"

#if defined (_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#endif

#include "Eval.h"

using namespace std;

#define LOCK_NODE(var) mutex_nodes[(var)].lock()
#define UNLOCK_NODE(var) mutex_nodes[(var)].unlock()
#define LOCK_EXPAND mutex_expand.lock();
#define UNLOCK_EXPAND mutex_expand.unlock();

typedef std::pair<std::wstring, std::vector<float>*> MapEntry;
typedef std::map<std::wstring, std::vector<float>*> Layer;

struct value_eval_req {
  child_node_t *uct_child;
  int color;
  int trans;
  std::vector<int> path;
  std::vector<float> data_basic;
  std::vector<float> data_features;
  std::vector<float> data_history;
};

struct policy_eval_req {
  int index;
  int depth;
  int color;
  int trans;
  std::vector<float> data_basic;
  std::vector<float> data_features;
  std::vector<float> data_history;
};

void ReadWeights();
void EvalNode();
//void EvalUctNode(std::vector<int>& indices, std::vector<int>& color, std::vector<int>& trans, std::vector<float>& data, std::vector<int>& path);

////////////////
//  大域変数  //
////////////////

// 持ち時間
double remaining_time[S_MAX];

// UCTのノード
uct_node_t *uct_node;

// プレイアウト情報
static po_info_t po_info;

// Progressive Widening の閾値
static int pw[PURE_BOARD_MAX + 1];  

// ノード展開の閾値
static int expand_threshold = EXPAND_THRESHOLD_19;

// ノードを展開しない
static bool no_expand = false;

// 試行時間を延長するかどうかのフラグ
static bool extend_time = false;

int current_root; // 現在のルートのインデックス
mutex mutex_nodes[MAX_NODES];
mutex mutex_expand;       // ノード展開を排他処理するためのmutex

// 探索の設定
enum SEARCH_MODE mode = CONST_TIME_MODE;
// 使用するスレッド数
int threads = 1;
// 1手あたりの試行時間
double const_thinking_time = CONST_TIME;
// 1手当たりのプレイアウト数
int playout = CONST_PLAYOUT;
// デフォルトの持ち時間
double default_remaining_time = ALL_THINKING_TIME;

// 各スレッドに渡す引数
thread_arg_t t_arg[THREAD_MAX];

// プレイアウトの統計情報
statistic_t statistic[BOARD_MAX];  
// 盤上の各点のCriticality
double criticality[BOARD_MAX];  
// 盤上の各点のOwner(0-100%)
double owner[BOARD_MAX];

// 現在のオーナーのインデックス
int owner_index[BOARD_MAX];   
// 現在のクリティカリティのインデックス
int criticality_index[BOARD_MAX];  

// 候補手のフラグ
bool candidates[BOARD_MAX];  

bool pondering_mode = false;

bool ponder = false;

bool pondering_stop = false;

bool pondered = false;

double time_limit;

std::thread *handle[THREAD_MAX];    // スレッドのハンドル

// UCB Bonusの等価パラメータ
double bonus_equivalence = BONUS_EQUIVALENCE;
// UCB Bonusの重み
double bonus_weight = BONUS_WEIGHT;

// 乱数生成器
std::mt19937_64 *mt[THREAD_MAX];

// Last-Good-Reply
LGR lgr;
std::vector<LGRContext> lgr_ctx;

// Criticalityの上限値
int criticality_max = CRITICALITY_MAX;

// 
bool reuse_subtree = false;

// 自分の手番の色
int my_color;

const double pass_po_limit = 0.5;
const int policy_batch_size = 16;
const int value_batch_size = 64;

ray_clock::time_point begin_time;

static bool early_pass = true;

static bool use_nn = true;
static int device_id = 0;
static std::queue<std::shared_ptr<policy_eval_req>> eval_policy_queue;
static std::queue<std::shared_ptr<value_eval_req>> eval_value_queue;
static int eval_count_policy, eval_count_value;
static double owner_nn[BOARD_MAX];

static Microsoft::MSR::CNTK::IEvaluateModel<float>* nn_model = nullptr;

//template<double>
double atomic_fetch_add(std::atomic<double> *obj, double arg) {
  double expected = obj->load();
  while (!atomic_compare_exchange_weak(obj, &expected, expected + arg))
    ;
  return expected;
}

static void
ClearEvalQueue()
{
  queue<shared_ptr<value_eval_req>> empty_value;
  eval_value_queue.swap(empty_value);
  queue<shared_ptr<policy_eval_req>> empty_policy;
  eval_policy_queue.swap(empty_policy);
}

///////////////////
//
//
void
SetPonderingMode(bool flag)
{
  pondering_mode = flag;
}

////////////////////////
//  探索モードの指定  //
////////////////////////
void
SetMode(enum SEARCH_MODE new_mode)
{
  mode = new_mode;
}

///////////////////////////////////////
//  1手あたりのプレイアウト数の指定  //
///////////////////////////////////////
void
SetPlayout(int po)
{
  playout = po;
}


/////////////////////////////////
//  1手にかける試行時間の設定  //
/////////////////////////////////
void
SetConstTime(double time)
{
  const_thinking_time = time;
}


////////////////////////////////
//  使用するスレッド数の指定  //
////////////////////////////////
void
SetThread(int new_thread)
{
  threads = new_thread;

  lgr_ctx.resize(threads);
}


//////////////////////
//  持ち時間の設定  //
//////////////////////
void
SetTime(double time)
{
  default_remaining_time = time;
}


//////////////////////////
//  ノード再利用の設定  //
//////////////////////////
void
SetReuseSubtree(bool flag)
{
  reuse_subtree = flag;
}

//////////////////
//  パスの設定  //
//////////////////
void
SetEarlyPass(bool pass)
{
  early_pass = pass;
}


////////////////////////////////////////////
//  盤の大きさに合わせたパラメータの設定  //
////////////////////////////////////////////
void
SetParameter(void)
{
  if (pure_board_size < 11) {
    expand_threshold = EXPAND_THRESHOLD_9;
  } else if (pure_board_size < 16) {
    expand_threshold = EXPAND_THRESHOLD_13;
  } else {
    expand_threshold = EXPAND_THRESHOLD_19;
  }
}

////////////////////
//  NN利用の設定  //
////////////////////
void
SetUseNN(bool flag)
{
  use_nn = flag;
}

void
SetDeviceId(int id)
{
  device_id = id;
}

void
SetNoExpand(bool flag)
{
  no_expand = flag;
}

/////////////////////////
//  UCT探索の初期設定  //
/////////////////////////
void
InitializeUctSearch(void)
{
  int i;

  // Progressive Wideningの初期化  
  pw[0] = 0;
  for (i = 1; i <= PURE_BOARD_MAX; i++) {  
    pw[i] = pw[i - 1] + (int)(40 * pow(PROGRESSIVE_WIDENING, i - 1));
    if (pw[i] > 10000000) break;
  }
  for (i = i + 1; i <= PURE_BOARD_MAX; i++) { 
    pw[i] = INT_MAX;
  }

  // UCTのノードのメモリを確保
  uct_node = (uct_node_t *)malloc(sizeof(uct_node_t) * uct_hash_size);
  
  if (uct_node == NULL) {
    cerr << "Cannot allocate memory !!" << endl;
    cerr << "You must reduce tree size !!" << endl;
    exit(1);
  }

  if (use_nn && !nn_model)
    ReadWeights();
}


////////////////////////
//  探索設定の初期化  //
////////////////////////
void
InitializeSearchSetting(void)
{
  int i;

  // Ownerの初期化
  for (i = 0; i < board_max; i++){
    owner[i] = 50;
    owner_index[i] = 5;
    candidates[i] = true;
  }

  // 乱数の初期化
  for (i = 0; i < THREAD_MAX; i++) {
    if (mt[i]) {
      delete mt[i];
    }
    mt[i] = new mt19937_64((unsigned int)(time(NULL) + i));
  }

  // Initialize Last-Good-Reply
  lgr.reset();
  lgr_ctx.resize(threads);

  // 持ち時間の初期化
  for (i = 0; i < 3; i++) {
    remaining_time[i] = default_remaining_time;
  }

  // 制限時間を設定
  // プレイアウト回数の初期化
  if (mode == CONST_PLAYOUT_MODE) {
    time_limit = 100000.0;
    po_info.num = playout;
    extend_time = false;
  } else if (mode == CONST_TIME_MODE) {
    time_limit = const_thinking_time;
    po_info.num = 100000000;
    extend_time = false;
  } else if (mode == TIME_SETTING_MODE) {
    if (pure_board_size < 11) {
      time_limit = remaining_time[0] / TIME_RATE_9;
      po_info.num = (int)(PLAYOUT_SPEED * time_limit);
      extend_time = true;
    } else if (pure_board_size < 13) {
      time_limit = remaining_time[0] / (TIME_MAXPLY_13 + TIME_C_13);
      po_info.num = (int)(PLAYOUT_SPEED * time_limit);
      extend_time = true;
    } else {
      time_limit = remaining_time[0] / (TIME_MAXPLY_19 + TIME_C_19);
      po_info.num = (int)(PLAYOUT_SPEED * time_limit);
      extend_time = true;
    }
  }

  pondered = false;
  pondering_stop = true;
}


////////////
//  終了  //
////////////
void
FinalizeUctSearch(void)
{

}


bool
IsPondered()
{
  return pondered;
}

void
StopPondering()
{
  int i;

  if (!pondering_mode) {
    return;
  }

  if (ponder) {
    pondering_stop = true;
    for (i = 0; i < threads; i++) {
      handle[i]->join();
      delete handle[i];
      handle[i] = nullptr;
    }
    if (use_nn) {
      handle[threads]->join();
      delete handle[threads];
      handle[threads] = nullptr;
    }

    ponder = false;
    pondered = true;
    PrintPonderingCount(po_info.count);
  }
}


/////////////////////////////////////
//  UCTアルゴリズムによる着手生成  //
/////////////////////////////////////
int
UctSearchGenmove(game_info_t *game, int color)
{
  int i, pos;
  double finish_time;
  int select_index;
  int max_count;
  double pass_wp;
  double best_wp;
  child_node_t *uct_child;
  int pre_simulated;


  // 探索情報をクリア
  if (!pondered) {
    memset(statistic, 0, sizeof(statistic_t) * board_max); 
    memset(criticality_index, 0, sizeof(int) * board_max); 
    memset(criticality, 0, sizeof(double) * board_max);    
  }
  po_info.count = 0;

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    owner[pos] = 50;
    owner_index[pos] = 5;
    candidates[pos] = true;

    owner_nn[pos] = 50;
  }

  if (reuse_subtree) {
    DeleteOldHash(game);
  } else {
    ClearUctHash();
  }

  ClearEvalQueue();

  eval_count_policy = 0;
  eval_count_value = 0;

  // 探索開始時刻の記録
  begin_time = ray_clock::now();
  
  // UCTの初期化
  current_root = ExpandRoot(game, color);

  // 前回から持ち込んだ探索回数を記録
  pre_simulated = uct_node[current_root].move_count;

  // 子ノードが1つ(パスのみ)ならPASSを返す
  if (uct_node[current_root].child_num <= 1) {
    return PASS;
  }

  // 探索回数の閾値を設定
  po_info.halt = po_info.num;

  // 自分の手番を設定
  my_color = color;

  // Dynamic Komiの算出(置碁のときのみ)
  DynamicKomi(game, &uct_node[current_root], color);

  // 探索時間とプレイアウト回数の予定値を出力
  PrintPlayoutLimits(time_limit, po_info.halt);

  for (i = 0; i < threads; i++) {
    t_arg[i].thread_id = i;
    t_arg[i].game = game;
    t_arg[i].color = color;
    handle[i] = new thread(ParallelUctSearch, &t_arg[i]);
  }

  if (use_nn)
    handle[threads] = new thread(EvalNode);

  for (i = 0; i < threads; i++) {
    handle[i]->join();
    delete handle[i];
    handle[i] = nullptr;
  }
  if (use_nn) {
    handle[threads]->join();
    delete handle[threads];
    handle[threads] = nullptr;
  }

  // 着手が41手以降で, 
  // 時間延長を行う設定になっていて,
  // 探索時間延長をすべきときは
  // 探索回数を1.5倍に増やす
  if (game->moves > pure_board_size * 3 - 17 &&
      extend_time &&
      ExtendTime()) {
    po_info.halt = (int)(1.5 * po_info.halt);
    time_limit *= 1.5;
    for (i = 0; i < threads; i++) {
      handle[i] = new thread(ParallelUctSearch, &t_arg[i]);
    }
    if (use_nn)
      handle[threads] = new thread(EvalNode);

    for (i = 0; i < threads; i++) {
      handle[i]->join();
      delete handle[i];
      handle[i] = nullptr;
    }
    if (use_nn) {
      handle[threads]->join();
      delete handle[threads];
      handle[threads] = nullptr;
    }
  }

  uct_child = uct_node[current_root].child;

  select_index = PASS_INDEX;
  max_count = early_pass ? (int)uct_child[PASS_INDEX].move_count : 0;

  // 探索回数最大の手を見つける
  for (i = 1; i < uct_node[current_root].child_num; i++){
    if (uct_child[i].move_count > max_count) {
      select_index = i;
      max_count = uct_child[i].move_count;
    }
  }

  // 探索にかかった時間を求める
  finish_time = GetSpendTime(begin_time);

  // パスの勝率の算出
  if (uct_child[PASS_INDEX].move_count != 0) {
    pass_wp = (double)uct_child[PASS_INDEX].win / uct_child[PASS_INDEX].move_count;
  } else {
    pass_wp = 0;
  }

  // 選択した着手の勝率の算出(Dynamic Komi)
  best_wp = (double)uct_child[select_index].win / uct_child[select_index].move_count;

  // コミを含めない盤面のスコアを求める
  double score = (double)CalculateScore(game);
  // コミを考慮した勝敗
  score -= komi[my_color];

  // 各地点の領地になる確率の出力
  PrintOwner(&uct_node[current_root], color, owner);

  // 取れている石を数える
  int count = 0;
  for (i = 0; i < pure_board_max; i++) {
    int pos = onboard_pos[i];

    if (game->board[pos] == FLIP_COLOR(color) && owner[pos] > 70) {
      count++;
    }
  }

  // パスをするときは
  // 1. 直前の着手がパスで, パスした時の勝率がPASS_THRESHOLD以上
  //    early_pass か有効か死石をすべて打ち上げ済み
  // 2. 着手数がMAX_MOVES以上
  // 投了するときは
  //    Dynamic Komiでの勝率がRESIGN_THRESHOLD以下
  // それ以外は選ばれた着手を返す
  if (pass_wp >= PASS_THRESHOLD &&
      (early_pass || count == 0) &&
      (game->record[game->moves - 1].pos == PASS)){
    pos = PASS;
  } else if (game->moves >= MAX_MOVES) {
    pos = PASS;
  } else if (game->moves > 3 &&
             early_pass &&
	     game->record[game->moves - 1].pos == PASS &&
	     game->record[game->moves - 3].pos == PASS) {
    pos = PASS;
  } else if (best_wp <= RESIGN_THRESHOLD) {
    pos = RESIGN;
  } else {
    pos = uct_child[select_index].pos;
  }

  // 最善応手列を出力
  PrintBestSequence(game, uct_node, current_root, color);
  // 探索の情報を出力(探索回数, 勝敗, 思考時間, 勝率, 探索速度)
  PrintPlayoutInformation(&uct_node[current_root], &po_info, finish_time, pre_simulated);
  // 次の探索でのプレイアウト回数の算出
  CalculateNextPlayouts(game, color, best_wp, finish_time);

  ClearEvalQueue();
 
  if (use_nn && GetDebugMessageMode()) {
    cerr << "Eval NN Policy     :  " << setw(7) << (eval_count_policy + eval_policy_queue.size()) << endl;
    cerr << "Eval NN Value      :  " << setw(7) << (eval_count_value + eval_value_queue.size()) << endl;
    cerr << "Eval NN            :  " << setw(7) << eval_count_policy << "/" << eval_count_value << endl;
    cerr << "Count Captured     :  " << setw(7) << count << endl;
    cerr << "Score              :  " << setw(7) << score << endl;
    //PrintOwnerNN(S_BLACK, owner_nn);
  }

  return pos;
}


///////////////
//  予測読み  //
///////////////
void
UctSearchPondering(game_info_t *game, int color)
{
  int i, pos;

  if (!pondering_mode) {
    return ;
  }

  // 探索情報をクリア
  memset(statistic, 0, sizeof(statistic_t) * board_max);  
  memset(criticality_index, 0, sizeof(int) * board_max);  
  memset(criticality, 0, sizeof(double) * board_max);     
  po_info.count = 0;

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    owner[pos] = 50;
    owner_index[pos] = 5;
    candidates[pos] = true;
  }

  DeleteOldHash(game);

  // UCTの初期化
  current_root = ExpandRoot(game, color);

  pondered = false;

  // 子ノードが1つ(パスのみ)ならPASSを返す
  if (uct_node[current_root].child_num <= 1) {
    ponder = false;
    pondering_stop = true;
    return ;
  }

  ponder = true;
  pondering_stop = false;

  // Dynamic Komiの算出(置碁のときのみ)
  DynamicKomi(game, &uct_node[current_root], color);

  for (i = 0; i < threads; i++) {
    t_arg[i].thread_id = i;
    t_arg[i].game = game;
    t_arg[i].color = color;
    handle[i] = new thread(ParallelUctSearchPondering, &t_arg[i]);
  }

  if (use_nn)
    handle[threads] = new thread(EvalNode);

  return ;
}

/////////////////////////////////////
// 統計
/////////////////////////////////////
void
UctSearchStat(game_info_t *game, int color, int num)
{
  int i, pos;
  double finish_time;
  int select_index;
  int max_count;
  double pass_wp;
  double best_wp;
  child_node_t *uct_child;
  int pre_simulated;


  // 探索情報をクリア
  memset(statistic, 0, sizeof(statistic_t) * board_max); 
  memset(criticality_index, 0, sizeof(int) * board_max); 
  memset(criticality, 0, sizeof(double) * board_max);    
  po_info.count = 0;

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    owner[pos] = 50;
    owner_index[pos] = 5;
    candidates[pos] = true;
  }

  if (reuse_subtree) {
    DeleteOldHash(game);
  } else {
    ClearUctHash();
  }

  double org_use_nn = use_nn;
  use_nn = false;

  // 探索開始時刻の記録
  begin_time = ray_clock::now();

  // UCTの初期化
  current_root = ExpandRoot(game, color);

  // 前回から持ち込んだ探索回数を記録
  pre_simulated = uct_node[current_root].move_count;

  // 子ノードが1つ(パスのみ)ならPASSを返す
  if (uct_node[current_root].child_num <= 1) {
    return;
  }

  // 探索回数の閾値を設定
  po_info.halt = num;

  // 自分の手番を設定
  my_color = color;

  // Dynamic Komiの算出(置碁のときのみ)
  DynamicKomi(game, &uct_node[current_root], color);

  for (i = 0; i < threads; i++) {
    t_arg[i].thread_id = i;
    t_arg[i].game = game;
    t_arg[i].color = color;
    handle[i] = new thread(ParallelUctSearch, &t_arg[i]);
  }

  for (i = 0; i < threads; i++) {
    handle[i]->join();
    delete handle[i];
    handle[i] = nullptr;
  }

  use_nn = org_use_nn;

  uct_child = uct_node[current_root].child;

  select_index = PASS_INDEX;
  max_count = uct_child[PASS_INDEX].move_count;

  // 探索回数最大の手を見つける
  for (i = 1; i < uct_node[current_root].child_num; i++){
    if (uct_child[i].move_count > max_count) {
      select_index = i;
      max_count = uct_child[i].move_count;
    }
  }

  // 探索にかかった時間を求める
  finish_time = GetSpendTime(begin_time);

  // パスの勝率の算出
  if (uct_child[PASS_INDEX].move_count != 0) {
    pass_wp = (double)uct_child[PASS_INDEX].win / uct_child[PASS_INDEX].move_count;
  } else {
    pass_wp = 0;
  }

  // 選択した着手の勝率の算出(Dynamic Komi)
  best_wp = (double)uct_child[select_index].win / uct_child[select_index].move_count;

  cerr << (color == S_BLACK ? "BLACK" : "WHITE") << endl;
  // 各地点の領地になる確率の出力
  PrintOwner(&uct_node[current_root], color, owner);

  // 探索の情報を出力(探索回数, 勝敗, 思考時間, 勝率, 探索速度)
  PrintPlayoutInformation(&uct_node[current_root], &po_info, finish_time, 0);
}

/////////////////////
//  候補手の初期化  //
/////////////////////
void
InitializeCandidate(child_node_t *uct_child, int pos, bool ladder)
{
  uct_child->pos = pos;
  uct_child->move_count = 0;
  uct_child->win = 0;
  uct_child->eval_value = false;
  uct_child->index = NOT_EXPANDED;
  uct_child->rate = 0.0;
  uct_child->flag = false;
  uct_child->open = false;
  uct_child->ladder = ladder;
  uct_child->nnrate = 0;
  uct_child->value = -1;
}


/////////////////////////
//  ルートノードの展開  //
/////////////////////////
int
ExpandRoot(game_info_t *game, int color)
{
  unsigned int index = FindSameHashIndex(game->current_hash, color, game->moves);
  child_node_t *uct_child;
  int i, pos, child_num = 0;
  bool ladder[BOARD_MAX] = { false };  
  int pm1 = PASS, pm2 = PASS;
  int moves = game->moves;

  // 直前の着手の座標を取り出す
  pm1 = game->record[moves - 1].pos;
  // 2手前の着手の座標を取り出す
  if (moves > 1) pm2 = game->record[moves - 2].pos;

  // 9路盤でなければシチョウを調べる  
  if (pure_board_size != 9) {
    LadderExtension(game, color, ladder);
  }

  std::vector<int> path;

  // 既に展開されていた時は, 探索結果を再利用する
  if (index != uct_hash_size) {
    // 直前と2手前の着手を更新
    uct_node[index].previous_move1 = pm1;
    uct_node[index].previous_move2 = pm2;

    uct_child = uct_node[index].child;

    child_num = uct_node[index].child_num;

    for (i = 0; i < child_num; i++) {
      pos = uct_child[i].pos;
      uct_child[i].rate = 0.0;
      uct_child[i].flag = false;
      uct_child[i].open = false;
      if (ladder[pos]) {
	uct_node[index].move_count -= uct_child[i].move_count;
	uct_node[index].win -= uct_child[i].win;
	uct_child[i].move_count = 0;
	uct_child[i].win = 0;
	uct_child[i].eval_value = false;
      }
      uct_child[i].ladder = ladder[pos];
    }

    path.push_back(index);

    // 展開されたノード数を1に初期化
    uct_node[index].width = 1;

    // 候補手のレーティング
    RatingNode(game, color, index, path.size());

    PrintReuseCount(uct_node[index].move_count);

    return index;
  } else {
    // 空のインデックスを探す
    index = SearchEmptyIndex(game->current_hash, color, game->moves);

    assert(index != uct_hash_size);    
    
    // ルートノードの初期化
    uct_node[index].previous_move1 = pm1;
    uct_node[index].previous_move2 = pm2;
    uct_node[index].move_count = 0;
    uct_node[index].win = 0;
    uct_node[index].width = 0;
    uct_node[index].child_num = 0;
    uct_node[index].evaled = false;
    uct_node[index].value_move_count = 0;
    uct_node[index].value_win = 0;
    memset(uct_node[index].statistic, 0, sizeof(statistic_t) * BOARD_MAX); 
    memset(uct_node[index].seki, false, sizeof(bool) * BOARD_MAX);
    
    uct_child = uct_node[index].child;
    
    // パスノードの展開
    InitializeCandidate(&uct_child[PASS_INDEX], PASS, ladder[PASS]);
    child_num++;
    
    // 候補手の展開
    if (game->moves == 1) {
      for (i = 0; i < first_move_candidates; i++) {
	pos = first_move_candidate[i];
	// 探索候補かつ合法手であれば探索対象にする
	if (candidates[pos] && IsLegal(game, pos, color)) {
	  InitializeCandidate(&uct_child[child_num], pos, ladder[pos]);
	  child_num++;
	}	
      }
    } else {
      for (i = 0; i < pure_board_max; i++) {
	pos = onboard_pos[i];
	// 探索候補かつ合法手であれば探索対象にする
	if (candidates[pos] && IsLegal(game, pos, color)) {
	  InitializeCandidate(&uct_child[child_num], pos, ladder[pos]);
	  child_num++;
	}
      }
    }

    path.push_back(index);
    
    // 子ノード個数の設定
    uct_node[index].child_num = child_num;
    
    // 候補手のレーティング
    RatingNode(game, color, index, path.size());

    // セキの確認
    CheckSeki(game, uct_node[index].seki);
    
    uct_node[index].width++;
  }

  return index;
}



///////////////////
//  ノードの展開  //
///////////////////
int
ExpandNode(game_info_t *game, int color, int current, const std::vector<int>& path)
{
  unsigned int index = FindSameHashIndex(game->current_hash, color, game->moves);
  child_node_t *uct_child, *uct_sibling;
  int i, pos, child_num = 0;
  bool ladder[BOARD_MAX] = { false };  
  double max_rate = 0.0;
  int max_pos = PASS, sibling_num;
  int pm1 = PASS, pm2 = PASS;
  int moves = game->moves;

  // 合流先が検知できれば, それを返す
  if (index != uct_hash_size) {
    return index;
  }

  // 空のインデックスを探す
  index = SearchEmptyIndex(game->current_hash, color, game->moves);

  assert(index != uct_hash_size);    

  // 直前の着手の座標を取り出す
  pm1 = game->record[moves - 1].pos;
  // 2手前の着手の座標を取り出す
  if (moves > 1) pm2 = game->record[moves - 2].pos;

  // 9路盤でなければシチョウを調べる  
  if (pure_board_size != 9) {
    LadderExtension(game, color, ladder);
  }

  // 現在のノードの初期化
  uct_node[index].previous_move1 = pm1;
  uct_node[index].previous_move2 = pm2;
  uct_node[index].move_count = 0;
  uct_node[index].win = 0;
  uct_node[index].width = 0;
  uct_node[index].child_num = 0;
  uct_node[index].evaled = false;
  uct_node[index].value_move_count = 0;
  uct_node[index].value_win = 0;
  memset(uct_node[index].statistic, 0, sizeof(statistic_t) * BOARD_MAX);
  memset(uct_node[index].seki, false, sizeof(bool) * BOARD_MAX);
  
  uct_child = uct_node[index].child;

  // パスノードの展開
  InitializeCandidate(&uct_child[PASS_INDEX], PASS, ladder[PASS]);
  child_num++;

  // 候補手の展開
  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    // 探索候補でなければ除外
    if (candidates[pos] && IsLegal(game, pos, color)) {
      InitializeCandidate(&uct_child[child_num], pos, ladder[pos]);
      child_num++;
    }
  }

  // 子ノードの個数を設定
  uct_node[index].child_num = child_num;

  // 候補手のレーティング
  RatingNode(game, color, index, path.size() + 1);

  // セキの確認
  CheckSeki(game, uct_node[index].seki);
  
  // 探索幅を1つ増やす
  uct_node[index].width++;

  // 兄弟ノードで一番レートの高い手を求める
  uct_sibling = uct_node[current].child;
  sibling_num = uct_node[current].child_num;
  for (i = 0; i < sibling_num; i++) {
    if (uct_sibling[i].pos != pm1) {
      if (uct_sibling[i].rate > max_rate) {
	max_rate = uct_sibling[i].rate;
	max_pos = uct_sibling[i].pos;
      }
    }
  }

  // 兄弟ノードで一番レートの高い手を展開する
  for (i = 0; i < child_num; i++) {
    if (uct_child[i].pos == max_pos) {
      if (!uct_child[i].flag) {
	uct_child[i].open = true;
      }
      break;
    }
  }

  return index;
}


//////////////////////////////////////
//  ノードのレーティング             //
//  (Progressive Wideningのために)  //
//////////////////////////////////////
void
RatingNode(game_info_t *game, int color, int index, int depth)
{
  int i;
  int child_num = uct_node[index].child_num;
  int pos;
  int moves = game->moves;
  double score = 0.0;
  int max_index;
  double max_score;
  pattern_hash_t hash_pat;
  int pat_index[3] = {0};
  double dynamic_parameter;
  bool self_atari_flag;
  child_node_t *uct_child = uct_node[index].child;
  uct_features_t uct_features;

  memset(&uct_features, 0, sizeof(uct_features_t));

  // パスのレーティング
  uct_child[PASS_INDEX].rate = CalculateLFRScore(game, PASS, pat_index, &uct_features);

  // 直前の着手で発生した特徴の確認
  UctCheckFeatures(game, color, &uct_features);
  // 直前の着手で石を2つ取られたか確認
  UctCheckRemove2Stones(game, color, &uct_features);
  // 直前の着手で石を3つ取られたか確認
  UctCheckRemove3Stones(game, color, &uct_features);
  // 2手前で劫が発生していたら, 劫を解消するトリの確認
  if (game->ko_move == moves - 2) {
    UctCheckCaptureAfterKo(game, color, &uct_features);
    UctCheckKoConnection(game, &uct_features);
  }

  max_index = 0;
  max_score = uct_child[0].rate;

  if (use_nn) {
    //int color = game->record[game->moves - 1].color;

    uct_node_t *root = &uct_node[current_root];

    double rate[PURE_BOARD_MAX];
    AnalyzePoRating(game, color, rate);

    auto req = make_shared<policy_eval_req>();
    req->color = color;
    req->depth = depth;
    req->index = index;
    req->trans = rand() / (RAND_MAX / 8 + 1);
    //req.path.swap(path);
    WritePlanes(req->data_basic, req->data_features, req->data_history, nullptr,
      game, root, color, req->trans);
#if 1
    eval_policy_queue.push(req);
    //push_back(u);
#else
    std::vector<int> indices;
    indices.push_back(index);
    EvalUctNode(indices, req.data);
#endif
  }

  for (i = 1; i < child_num; i++) {
    pos = uct_child[i].pos;

    // 自己アタリの確認
    self_atari_flag = UctCheckSelfAtari(game, color, pos, &uct_features);
    // ウッテガエシの確認
    UctCheckSnapBack(game, color, pos, &uct_features);
    // トリの確認
    if ((uct_features.tactical_features1[pos] & capture_mask)== 0) {
      UctCheckCapture(game, color, pos, &uct_features);
    }
    // アタリの確認
    if ((uct_features.tactical_features1[pos] & atari_mask) == 0) {
      UctCheckAtari(game, color, pos, &uct_features);
    }
    // 両ケイマの確認
    UctCheckDoubleKeima(game, color, pos, &uct_features);
    // ケイマのツケコシの確認
    UctCheckKeimaTsukekoshi(game, color, pos, &uct_features);

    // 自己アタリが無意味だったらスコアを0.0にする
    // 逃げられないシチョウならスコアを-1.0にする
    if (!self_atari_flag) {
      score = 0.0;
    } else if (uct_child[i].ladder) {
      score = -1.0;
    } else {
#if 1
      // MD3, MD4, MD5のパターンのハッシュ値を求める
      PatternHash(&game->pat[pos], &hash_pat);
      // MD3のパターンのインデックスを探す
      pat_index[0] = SearchIndex(md3_index, hash_pat.list[MD_3]);
      // MD4のパターンのインデックスを探す
      pat_index[1] = SearchIndex(md4_index, hash_pat.list[MD_4]);
      // MD5のパターンのインデックスを探す
      pat_index[2] = SearchIndex(md5_index, hash_pat.list[MD_5 + MD_MAX]);

      score = CalculateLFRScore(game, pos, pat_index, &uct_features);
#else
      pos = uct_child[i].pos;
      int x = X(pos) - OB_SIZE;
      int y = Y(pos) - OB_SIZE;
      int n = x + y * pure_board_size;
      score = outputs[n] / sum;
      if (score > 0)
        uct_child[i].flag = true;
#endif
    }

    // その手のγを記録
    uct_child[i].rate = score;

    // 現在見ている箇所のOwnerとCriticalityの補正値を求める
    dynamic_parameter = uct_owner[owner_index[pos]] + uct_criticality[criticality_index[pos]];

    // 最もγが大きい着手を記録する
    if (score + dynamic_parameter > max_score) {
      max_index = i;
      max_score = score + dynamic_parameter;
    }

    // ウッテガエシだったら強制的に探索候補に入れる
    if ((uct_features.tactical_features1[pos] & uct_mask[UCT_SNAPBACK]) > 0) {
      uct_child[i].open = true;
    }

    // オイオトシだったら強制的に探索候補に入れる
    if ((uct_features.tactical_features1[pos] & uct_mask[UCT_OIOTOSHI]) > 0) {
      uct_child[i].open = true;
    }

  }

  // 最もγが大きい着手を探索できるようにする
  uct_child[max_index].flag = true;
}




//////////////////////////
//  探索打ち止めの確認  //
//////////////////////////
bool
InterruptionCheck(void)
{
  int i;
  int max = 0, second = 0;
  int child_num = uct_node[current_root].child_num;
  int rest = po_info.halt - po_info.count;
  child_node_t *uct_child = uct_node[current_root].child;

  if (mode != CONST_PLAYOUT_MODE && 
      GetSpendTime(begin_time) * 10.0 < time_limit) {
      return false;
  }

  // 探索回数が最も多い手と次に多い手を求める
  for (i = 0; i < child_num; i++) {
    if (uct_child[i].move_count > max) {
      second = max;
      max = uct_child[i].move_count;
    } else if (uct_child[i].move_count > second) {
      second = uct_child[i].move_count;
    }
  }

  // 残りの探索を全て次善手に費やしても
  // 最善手を超えられない場合は探索を打ち切る
  if (max - second > rest) {
    return true;
  } else {
    return false;
  }
}


///////////////////////////
//  思考時間延長の確認   //
///////////////////////////
bool
ExtendTime(void)
{
  int i;
  int max = 0, second = 0;
  int child_num = uct_node[current_root].child_num;
  child_node_t *uct_child = uct_node[current_root].child;

  // 探索回数が最も多い手と次に多い手を求める
  for (i = 0; i < child_num; i++) {
    if (uct_child[i].move_count > max) {
      second = max;
      max = uct_child[i].move_count;
    } else if (uct_child[i].move_count > second) {
      second = uct_child[i].move_count;
    }
  }

  // 最善手の探索回数がが次善手の探索回数の
  // 1.2倍未満なら探索延長
  if (max < second * 1.2) {
    return true;
  } else {
    return false;
  }
}



/////////////////////////////////
//  並列処理で呼び出す関数     //
//  UCTアルゴリズムを反復する  //
/////////////////////////////////
void
ParallelUctSearch(thread_arg_t *arg)
{
  static std::atomic<int> queue_full;
  thread_arg_t *targ = (thread_arg_t *)arg;
  game_info_t *game;
  int color = targ->color;
  bool interruption = false;
  bool enough_size = true;
  int winner = 0;
  int interval = CRITICALITY_INTERVAL;
  bool seki[BOARD_MAX] = {false};
  
  game = AllocateGame();

  CheckSeki(targ->game, seki);
  
  // スレッドIDが0のスレッドだけ別の処理をする
  // 探索回数が閾値を超える, または探索が打ち切られたらループを抜ける
  if (targ->thread_id == 0) {
    do {
      // Wait if dcnn queue is full
      LOCK_EXPAND;
      while (eval_value_queue.size() > value_batch_size * 3 || eval_policy_queue.size() > policy_batch_size * 3) {
	std::atomic_fetch_add(&queue_full, 1);
	UNLOCK_EXPAND;
	this_thread::sleep_for(chrono::milliseconds(10));
	if (queue_full % 1000 == 0)
	  cerr << "EVAL QUEUE FULL" << endl;
	LOCK_EXPAND;
      }
      UNLOCK_EXPAND;
      // 探索回数を1回増やす	
      atomic_fetch_add(&po_info.count, 1);
      // 盤面のコピー
      CopyGame(game, targ->game);
      memcpy(game->seki, seki, sizeof(bool) * BOARD_MAX);
      // 1回プレイアウトする
      //double value_result = -1;
      std::vector<int> path;
      UctSearch(game, color, mt[targ->thread_id], lgr, lgr_ctx[targ->thread_id], current_root, &winner, path);
      // 探索を打ち切るか確認
      interruption = InterruptionCheck();
      // ハッシュに余裕があるか確認
      enough_size = CheckRemainingHashSize();
      // OwnerとCriticalityを計算する
      if (po_info.count > interval) {
	CalculateOwner(color, po_info.count);
	CalculateCriticality(color);
	interval += CRITICALITY_INTERVAL;
      }
      if (GetSpendTime(begin_time) > time_limit) break;
    } while (po_info.count < po_info.halt && !interruption && enough_size);
  } else {
    do {
      // 探索回数を1回増やす	
      atomic_fetch_add(&po_info.count, 1);
      // 盤面のコピー
      CopyGame(game, targ->game);
      memcpy(game->seki, seki, sizeof(bool) * BOARD_MAX);
      // 1回プレイアウトする
      //double value_result = -1;
	  std::vector<int> path;
      UctSearch(game, color, mt[targ->thread_id], lgr, lgr_ctx[targ->thread_id], current_root, &winner, path);
      // 探索を打ち切るか確認
      interruption = InterruptionCheck();
      // ハッシュに余裕があるか確認
      enough_size = CheckRemainingHashSize();
      if (GetSpendTime(begin_time) > time_limit) break;
    } while (po_info.count < po_info.halt && !interruption && enough_size);
  }

  // メモリの解放
  FreeGame(game);
  return;
}


/////////////////////////////////
//  並列処理で呼び出す関数     //
//  UCTアルゴリズムを反復する  //
/////////////////////////////////
void
ParallelUctSearchPondering(thread_arg_t *arg)
{
  thread_arg_t *targ = (thread_arg_t *)arg;
  game_info_t *game;
  int color = targ->color;
  bool enough_size = true;
  int winner = 0;
  int interval = CRITICALITY_INTERVAL;

  game = AllocateGame();

  // スレッドIDが0のスレッドだけ別の処理をする
  // 探索回数が閾値を超える, または探索が打ち切られたらループを抜ける
  if (targ->thread_id == 0) {
    do {
      // 探索回数を1回増やす	
      atomic_fetch_add(&po_info.count, 1);
      // 盤面のコピー
      CopyGame(game, targ->game);
      // 1回プレイアウトする
      //double value_result = -1;
      std::vector<int> path;
      UctSearch(game, color, mt[targ->thread_id], lgr, lgr_ctx[targ->thread_id], current_root, &winner, path);
      // ハッシュに余裕があるか確認
      enough_size = CheckRemainingHashSize();
      // OwnerとCriticalityを計算する
      if (po_info.count > interval) {
	CalculateOwner(color, po_info.count);
	CalculateCriticality(color);
	interval += CRITICALITY_INTERVAL;
      }
    } while (!pondering_stop && enough_size);
  } else {
    do {
      // 探索回数を1回増やす	
      atomic_fetch_add(&po_info.count, 1);
      // 盤面のコピー
      CopyGame(game, targ->game);
      // 1回プレイアウトする
      //double value_result = -1;
      std::vector<int> path;
      UctSearch(game, color, mt[targ->thread_id], lgr, lgr_ctx[targ->thread_id], current_root, &winner, path);
      // ハッシュに余裕があるか確認
      enough_size = CheckRemainingHashSize();
    } while (!pondering_stop && enough_size);
  }

  // メモリの解放
  FreeGame(game);
  return;
}


//////////////////////////////////////////////
//  UCT探索を行う関数                        //
//  1回の呼び出しにつき, 1プレイアウトする    //
//////////////////////////////////////////////
int 
UctSearch(game_info_t *game, int color, mt19937_64 *mt, LGR& lgrf, LGRContext& lgrctx, int current, int *winner, std::vector<int>& path)
{
  int result = 0, next_index;
  double score;
  child_node_t *uct_child = uct_node[current].child;  

  // 現在見ているノードをロック
  LOCK_NODE(current);
  // UCB値最大の手を求める
  next_index = SelectMaxUcbChild(game, current, color);
  // Store context hash
  if (uct_child[next_index].move_count < 10) {
    lgrctx.store(game, PASS);
  } else {
    lgrctx.store(game, uct_child[next_index].pos);
  }
  // 選んだ手を着手
  PutStone(game, uct_child[next_index].pos, color);
  // 色を入れ替える
  color = FLIP_COLOR(color);

  bool end_of_game = game->moves > 2 &&
    game->record[game->moves - 1].pos == PASS &&
    game->record[game->moves - 2].pos == PASS;

  if (no_expand || uct_child[next_index].move_count < expand_threshold || end_of_game) {
    int start = game->moves;
    path.push_back(current);

    // Virtual Lossを加算
    AddVirtualLoss(&uct_child[next_index], current);

    memcpy(game->seki, uct_node[current].seki, sizeof(bool) * BOARD_MAX);
    
    // 現在見ているノードのロックを解除
    UNLOCK_NODE(current);

    // Enqueue value

    bool expected = false;
    if (use_nn
      && atomic_compare_exchange_strong(&uct_child[next_index].eval_value, &expected, true)) {

      uct_node_t *root = &uct_node[current_root];

      double rate[PURE_BOARD_MAX];
      AnalyzePoRating(game, color, rate);
      auto req = make_shared<value_eval_req>();
      req->uct_child = uct_child + next_index;
      req->color = color;
      //req->index = index;
      req->trans = rand() / (RAND_MAX / 8 + 1);
      req->path.swap(path);
      WritePlanes(req->data_basic, req->data_features, req->data_history, nullptr,
        game, root, color, req->trans);
      LOCK_EXPAND;
      eval_value_queue.push(req);
      UNLOCK_EXPAND;
    }

    // 終局まで対局のシミュレーション
    Simulation(game, color, mt, lgr, lgrctx);
    
    // コミを含めない盤面のスコアを求める
    score = (double)CalculateScore(game);
    
    // コミを考慮した勝敗
    if (score - dynamic_komi[my_color] > 0) {
      result = (color == S_BLACK ? 0 : 1);
      *winner = S_BLACK;
    } else if (score - dynamic_komi[my_color] < 0){
      result = (color == S_WHITE ? 0 : 1);
      *winner = S_WHITE;
    }
    
    // 統計情報の記録
    Statistic(game, *winner);

    lgr.update(game, start, *winner, lgrctx);
  } else {
    path.push_back(current);
    // Virtual Lossを加算
    AddVirtualLoss(&uct_child[next_index], current);
    // ノードの展開の確認
    if (uct_child[next_index].index == -1) {
      // ノードの展開中はロック
      LOCK_EXPAND;
      // ノードの展開
      uct_child[next_index].index = ExpandNode(game, color, current, path);
      //cerr << "value evaluated " << result << " " << v << " " << *value_result << endl;
      // ノード展開のロックの解除
      UNLOCK_EXPAND;
    }
    // 現在見ているノードのロックを解除
    UNLOCK_NODE(current);
    // 手番を入れ替えて1手深く読む
    result = UctSearch(game, color, mt, lgrf, lgrctx, uct_child[next_index].index, winner, path);
    //
    // double v = uct_node[current].value;
    // if (*value_result < 0 && v >= 0) {
    //   *value_result = color == S_WHITE ? v : 1 - v;
    // }
  }
#if 0
  double v = uct_node[current].value.load();
  if (v >= 0) {
    cerr << "value evaluated " << result << " " << v << " " << *value_result << endl;
  }
  if (*value_result < 0 && v >= 0 && atomic_compare_exchange_strong(&uct_node[current].value, &v, -2.0)) {
    *value_result = color == S_BLACK ? 1 - v : v;
    cerr << "update value " << result << " " << v << endl;
  }
#endif

  // 探索結果の反映
  UpdateResult(&uct_child[next_index], result, current);

  // 統計情報の更新
  UpdateNodeStatistic(game, *winner, uct_node[current].statistic);

  // if (*value_result >= 0)
  // 	*value_result = 1 - *value_result;
  return 1 - result;
}


//////////////////////////
//  Virtual Lossの加算  //
//////////////////////////
void
AddVirtualLoss(child_node_t *child, int current)
{
#if defined CPP11
  atomic_fetch_add(&uct_node[current].move_count, VIRTUAL_LOSS);
  atomic_fetch_add(&child->move_count, VIRTUAL_LOSS);
#else
  uct_node[current].move_count += VIRTUAL_LOSS;
  child->move_count += VIRTUAL_LOSS;
#endif
}


//////////////////////
//  探索結果の更新  //
/////////////////////
void
UpdateResult(child_node_t *child, int result, int current)
{
  atomic_fetch_add(&uct_node[current].win, result);
  atomic_fetch_add(&uct_node[current].move_count, 1 - VIRTUAL_LOSS);
  atomic_fetch_add(&child->win, result);
  atomic_fetch_add(&child->move_count, 1 - VIRTUAL_LOSS);
  // if (value >= 0) {
  //   atomic_fetch_add(&uct_node[current].value_win, value);
  //   atomic_fetch_add(&uct_node[current].value_move_count, 1);
  // }
}


//////////////////////////
//  ノードの並び替え用  //
//////////////////////////
int
RateComp(const void *a, const void *b)
{
  rate_order_t *ro1 = (rate_order_t *)a;
  rate_order_t *ro2 = (rate_order_t *)b;
  if (ro1->rate < ro2->rate) {
    return 1;
  } else if (ro1->rate > ro2->rate) {
    return -1;
  } else {
    return 0;
  }
}


///////////////////////////////////////////////////////
//  UCBが最大となる子ノードのインデックスを返す関数  //
///////////////////////////////////////////////////////
int
SelectMaxUcbChild(const game_info_t *game, int current, int color)
{
  bool evaled = uct_node[current].evaled;
  child_node_t *uct_child = uct_node[current].child;
  int child_num = uct_node[current].child_num;
  int max_child = 0, sum = uct_node[current].move_count;
  double p, max_value;
  double ucb_value;
  int max_index;
  double max_rate;
  double dynamic_parameter;
  rate_order_t order[PURE_BOARD_MAX + 1];  
  int width;
  double ucb_bonus_weight = bonus_weight * sqrt(bonus_equivalence / (sum + bonus_equivalence));
  const bool debug = current == current_root && sum % 10000 == 0 && GetDebugMessageMode();

  {
    // 128回ごとにOwnerとCriticalityでソートし直す  
    if ((sum & 0x7f) == 0 && sum != 0) {
      int o_index[UCT_CHILD_MAX], c_index[UCT_CHILD_MAX];
      CalculateCriticalityIndex(&uct_node[current], uct_node[current].statistic, color, c_index);
      CalculateOwnerIndex(&uct_node[current], uct_node[current].statistic, color, o_index);
      for (int i = 0; i < child_num; i++) {
	int pos = uct_child[i].pos;
	dynamic_parameter = uct_owner[o_index[i]] + uct_criticality[c_index[i]];
	order[i].rate = uct_child[i].rate + dynamic_parameter;
	order[i].index = i;
	uct_child[i].flag |= uct_child[i].nnrate > 0.01;
      }
      qsort(order, child_num, sizeof(rate_order_t), RateComp);

      // 子ノードの数と探索幅の最小値を取る
      width = ((uct_node[current].width > child_num) ? child_num : uct_node[current].width);

      // 探索候補の手を展開し直す
      for (int i = 0; i < width; i++) {
	uct_child[order[i].index].flag = true;
      }
    }

    // Progressive Wideningの閾値を超えたら, 
    // レートが最大の手を読む候補を1手追加
    if (sum > pw[uct_node[current].width]) {
      max_index = -1;
      max_rate = 0;
      for (int i = 0; i < child_num; i++) {
	if (uct_child[i].flag == false) {
	  int pos = uct_child[i].pos;
	  dynamic_parameter = uct_owner[owner_index[pos]] + uct_criticality[criticality_index[pos]];
	  if (uct_child[i].rate + dynamic_parameter > max_rate) {
	    max_index = i;
	    max_rate = uct_child[i].rate + dynamic_parameter;
	  }
	}
      }
      if (max_index != -1) {
	uct_child[max_index].flag = true;
      }
      uct_node[current].width++;
    }
  }

  max_value = -1;
  max_child = 0;

  const double p_p = (double)uct_node[current].win / uct_node[current].move_count;
  const double p_v = (double)uct_node[current].value_win / (uct_node[current].value_move_count + .01);
  const double scale = std::max(0.2, std::min(1.0, 1.0 - (game->moves - 200) / 50.0)) * value_scale;

  int start_child = 0;
  if (!early_pass && current == current_root && child_num > 1) {
    if (uct_child[0].move_count > uct_node[current].move_count * pass_po_limit) {
      start_child = 1;
    }
  }
  // UCB値最大の手を求める  
  for (int i = start_child; i < child_num; i++) {
    if (uct_child[i].flag || uct_child[i].open) {
      //double p2 = -1;
      double value_win = 0;
      double value_move_count = 0;

#if 1
      if (uct_child[i].index >= 0 && i != 0) {
	auto node = &uct_node[uct_child[i].index];
	if (node->value_move_count > 0) {
	  //p2 = 1 - (double)node->value_win / node->value_move_count;
	  value_win = node->value_win;
	  value_move_count = node->value_move_count;
	  value_win = value_move_count - value_win;
	}
	//cerr << "VA:" << (value_win / value_move_count) << " VS:" << uct_child[i].value << endl;
      }
      if (value_move_count == 0 && uct_child[i].value >= 0) {
	value_move_count = 1;
	value_win = uct_child[i].value;
      }
#endif
      double win = uct_child[i].win;
      double move_count = uct_child[i].move_count;

      if (evaled) {
	if (debug) {
	   cerr << uct_node[current].move_count << ".";
	   cerr << setw(3) << FormatMove(uct_child[i].pos);
	   cerr << ": move " << setw(5) << move_count << " policy "
	    << setw(10) << (uct_child[i].nnrate  * 100) << " ";
	}
	if (move_count == 0) {
	  p = p_p * (1 - scale) + p_v * scale;
	} else {
	  double p0 = win / move_count;
	  if (value_move_count > 0) {
	    double p1 = value_win / value_move_count;
	    //p = (uct_child[i].win + value_win) / (uct_child[i].move_count + value_move_count);
	    p = p0 * (1 - scale)  + p1 * scale;
	    //p = (p0 + p1) / 2;
	    //if (current == current_root) cerr << i << ":" << p0 << " " << p1 << " => " << p << endl;
	    if (debug) {
		cerr << "DP:" << setw(10) << (p0 * 100) << " DV:" << setw(10) << (p1 * 100) << " => " << setw(10) << (p * 100)
		<< " " << p_v << " V:" << (value_win / value_move_count);
		cerr << " LM:" << scale << " ";
	    }
	  } else {
	    p = p0 * (1 - scale) + p_v * scale;
	  }
	}

	double u = sqrt(sum) / (1 + uct_child[i].move_count);
	double rate = max(uct_child[i].nnrate, 0.01);
	ucb_value = p + c_puct * u * rate;

	if (debug) {
	  cerr << " P:" << p << " UCB:" << ucb_value << endl;
	}
      } else {
	if (uct_child[i].move_count == 0) {
	  ucb_value = FPU;
	} else {
	  double div, v;
	  // UCB1-TUNED value
	  p = (double) uct_child[i].win / uct_child[i].move_count;
	  //if (p2 >= 0) p = (p * 9 + p2) / 10;
	  div = log(sum) / uct_child[i].move_count;
	  v = p - p * p + sqrt(2.0 * div);
	  ucb_value = p + sqrt(div * ((0.25 < v) ? 0.25 : v));

	  // UCB Bonus
	  ucb_value += ucb_bonus_weight * uct_child[i].rate;
	}
      }

      if (ucb_value > max_value) {
	max_value = ucb_value;
	max_child = i;
      }
    }
  }

  return max_child;
}


///////////////////////////////////////////////////////////
//  OwnerやCriiticalityを計算するための情報を記録する関数  //
///////////////////////////////////////////////////////////
void
Statistic(game_info_t *game, int winner)
{
  char *board = game->board;
  int i, pos, color;

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    color = board[pos];
    if (color == S_EMPTY) color = territory[Pat3(game->pat, pos)];

    std::atomic_fetch_add(&statistic[pos].colors[color], 1);
    if (color == winner) {
      std::atomic_fetch_add(&statistic[pos].colors[0], 1);
    }
  }
}


///////////////////////////////
//  各ノードの統計情報の更新  //
///////////////////////////////
void
UpdateNodeStatistic(game_info_t *game, int winner, statistic_t *node_statistic)
{
  char *board = game->board;
  int i, pos, color;

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    color = board[pos];
    if (color == S_EMPTY) color = territory[Pat3(game->pat, pos)];
    std::atomic_fetch_add(&node_statistic[pos].colors[color], 1);
    if (color == winner) {
      std::atomic_fetch_add(&node_statistic[pos].colors[0], 1);
    }
  }
}


//////////////////////////////////
//  各ノードのCriticalityの計算  //
//////////////////////////////////
void
CalculateCriticalityIndex(uct_node_t *node, statistic_t *node_statistic, int color, int *index)
{
  double win, lose;
  int other = FLIP_COLOR(color);
  int count = node->move_count;
  int child_num = node->child_num;
  int i, pos;
  double tmp;

  win = (double)node->win / node->move_count;
  lose = 1.0 - win;

  index[0] = 0;

  for (i = 1; i < child_num; i++) {
    pos = node->child[i].pos;

    tmp = ((double)node_statistic[pos].colors[0] / count) -
      ((((double)node_statistic[pos].colors[color] / count) * win)
       + (((double)node_statistic[pos].colors[other] / count) * lose));
    if (tmp < 0) tmp = 0;
    index[i] = (int)(tmp * 40);
    if (index[i] > criticality_max - 1) index[i] = criticality_max - 1;
  }
}

////////////////////////////////////
//  Criticalityの計算をする関数   // 
////////////////////////////////////
void
CalculateCriticality(int color)
{
  int i, pos;
  double tmp;
  int other = FLIP_COLOR(color);
  double win, lose;

  win = (double)uct_node[current_root].win / uct_node[current_root].move_count;
  lose = 1.0 - win;

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];

    tmp = ((float)statistic[pos].colors[0] / po_info.count) -
      ((((float)statistic[pos].colors[color] / po_info.count)*win)
       + (((float)statistic[pos].colors[other] / po_info.count)*lose));
    criticality[pos] = tmp;
    if (tmp < 0) tmp = 0;
    criticality_index[pos] = (int)(tmp * 40);
    if (criticality_index[pos] > criticality_max - 1) criticality_index[pos] = criticality_max - 1;
  }
}



//////////////////////////////
//  Ownerの計算をする関数   //
//////////////////////////////
void
CalculateOwnerIndex( uct_node_t *node, statistic_t *node_statistic, int color, int *index )
{
  int i, pos;
  int count = node->move_count;
  int child_num = node->child_num;

  index[0] = 0;

  for (i = 1; i < child_num; i++){
    pos = node->child[i].pos;
    index[i] = (int)((double)node_statistic[pos].colors[color] * 10.0 / count + 0.5);
    if (index[i] > OWNER_MAX - 1) index[i] = OWNER_MAX - 1;
    if (index[i] < 0)   index[pos] = 0;
  }
}




//////////////////////////////
//  Ownerの計算をする関数   //
//////////////////////////////
void
CalculateOwner( int color, int count )
{
  int i, pos;

  for (i = 0; i < pure_board_max; i++){
    pos = onboard_pos[i];
    owner_index[pos] = (int)((double)statistic[pos].colors[color] * 10.0 / count + 0.5);
    if (owner_index[pos] > OWNER_MAX - 1) owner_index[pos] = OWNER_MAX - 1;
    if (owner_index[pos] < 0)   owner_index[pos] = 0;
  }
}


/////////////////////////////////
//  次のプレイアウト回数の設定  //
/////////////////////////////////
void
CalculateNextPlayouts( game_info_t *game, int color, double best_wp, double finish_time )
{
  double po_per_sec;

  if (finish_time != 0.0) {
    po_per_sec = po_info.count / finish_time;
  } else {
    po_per_sec = PLAYOUT_SPEED * threads;
  }

  // 次の探索の時の探索回数を求める
  if (mode == CONST_TIME_MODE) {
    if (best_wp > 0.90) {
      po_info.num = (int)(po_info.count / finish_time * const_thinking_time / 2);
    } else {
      po_info.num = (int)(po_info.count / finish_time * const_thinking_time);
    }
  } else if (mode == TIME_SETTING_MODE) {
    if (pure_board_size < 11) {
      remaining_time[color] -= finish_time;
      time_limit = remaining_time[color] / TIME_RATE_9;
      po_info.num = (int)(po_per_sec * time_limit);
    } else if (pure_board_size < 16) {
      remaining_time[color] -= finish_time;
      time_limit = remaining_time[color] / (TIME_C_13 + ((TIME_MAXPLY_13 - (game->moves + 1) > 0) ? TIME_MAXPLY_13 - (game->moves + 1) : 0));
      po_info.num = po_per_sec * time_limit;
    } else {
      remaining_time[color] -= finish_time;
      time_limit = remaining_time[color] / (TIME_C_19 + ((TIME_MAXPLY_19 - (game->moves + 1) > 0) ? TIME_MAXPLY_19 - (game->moves + 1) : 0));
      po_info.num = po_per_sec * time_limit;
    }
  }
}


/////////////////////////////////////
//  UCTアルゴリズムによる局面解析  //
/////////////////////////////////////
int
UctAnalyze( game_info_t *game, int color )
{
  int i, pos;
  thread *handle[THREAD_MAX];

  // 探索情報をクリア
  memset(statistic, 0, sizeof(statistic_t) * board_max);  
  memset(criticality_index, 0, sizeof(int) * board_max);  
  memset(criticality, 0, sizeof(double) * board_max);     
  po_info.count = 0;

  ClearUctHash();

  current_root = ExpandRoot(game, color);

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
  }

  double org_use_nn = use_nn;
  use_nn = false;

  po_info.halt = 10000;

  for (i = 0; i < threads; i++) {
    t_arg[i].thread_id = i;
    t_arg[i].game = game;
    t_arg[i].color = color;
    handle[i] = new std::thread(ParallelUctSearch, &t_arg[i]);
  }


  for (i = 0; i < threads; i++) {
    handle[i]->join();
    delete handle[i];
    handle[i] = nullptr;
  }

  use_nn = org_use_nn;

  int x, y, black = 0, white = 0;
  double own;

  for (y = board_start; y <= board_end; y++) {
    for (x = board_start; x <= board_end; x++) {
      pos = POS(x, y);
      own = (double)statistic[pos].colors[S_BLACK] / uct_node[current_root].move_count;
      if (own > 0.5) {
	black++;
      } else {
	white++;
      }
    }
  }

  PrintOwner(&uct_node[current_root], color, owner);

  return black - white;
}


/////////////////////////
//  Ownerをコピーする  //
/////////////////////////
void
OwnerCopy( int *dest )
{
  int i, pos;
  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    dest[pos] = (int)((double)uct_node[current_root].statistic[pos].colors[my_color] / uct_node[current_root].move_count * 100);
  }
}


///////////////////////////////
//  Criticalityをコピーする  //
///////////////////////////////
void
CopyCriticality( double *dest )
{
  int i, pos;
  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    dest[pos] = criticality[pos];
  }
}

void
CopyStatistic( statistic_t *dest )
{
  memcpy(dest, statistic, sizeof(statistic_t)* BOARD_MAX); 
}


////////////////////////////////////////////////////////
//  UCTアルゴリズムによる着手生成(KGS Clean Up Mode)  //
////////////////////////////////////////////////////////
int
UctSearchGenmoveCleanUp( game_info_t *game, int color )
{
  int i, pos;
  double finish_time;
  int select_index;
  int max_count;
  double wp;
  int count;
  child_node_t *uct_child;
  thread *handle[THREAD_MAX];

  memset(statistic, 0, sizeof(statistic_t)* board_max); 
  memset(criticality_index, 0, sizeof(int)* board_max); 
  memset(criticality, 0, sizeof(double)* board_max);    

  begin_time = ray_clock::now();

  po_info.count = 0;

  current_root = ExpandRoot(game, color);

  if (uct_node[current_root].child_num <= 1) {
    pos = PASS;
    return pos;
  }

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];
    owner[pos] = 50.0;
  }

  po_info.halt = po_info.num;

  DynamicKomi(game, &uct_node[current_root], color);

  for (i = 0; i < threads; i++) {
    t_arg[i].thread_id = i;
    t_arg[i].game = game;
    t_arg[i].color = color;
    handle[i] = new std::thread(ParallelUctSearch, &t_arg[i]);
  }

  for (i = 0; i < threads; i++) {
    handle[i]->join();
    delete handle[i];
    handle[i] = nullptr;
  }

  uct_child = uct_node[current_root].child;

  select_index = 0;
  max_count = uct_child[0].move_count;

  for (i = 0; i < uct_node[current_root].child_num; i++){
    if (uct_child[i].move_count > max_count) {
      select_index = i;
      max_count = uct_child[i].move_count;
    }
  }

  finish_time = GetSpendTime(begin_time);

  wp = (double)uct_node[current_root].win / uct_node[current_root].move_count;

  PrintPlayoutInformation(&uct_node[current_root], &po_info, finish_time, 0);
  PrintOwner(&uct_node[current_root], color, owner);

  pos = uct_child[select_index].pos;

  PrintBestSequence(game, uct_node, current_root, color);

  CalculateNextPlayouts(game, color, wp, finish_time);

  count = 0;

  for (i = 0; i < pure_board_max; i++) {
    pos = onboard_pos[i];

    if (owner[pos] >= 5 || owner[pos] <= 95) {
      candidates[pos] = true;
      count++;
    } else {
      candidates[pos] = false;
    }
  }

  if (count == 0) pos = PASS;
  else pos = uct_child[select_index].pos;

  if ((double)uct_child[select_index].win / uct_child[select_index].move_count < RESIGN_THRESHOLD) {
    pos = PASS;
  }

  return pos;
}

extern char uct_params_path[1024];

void
ReadWeights()
{
  cerr << "Init CNTK" << endl;
  GetEvalF(&nn_model);
  if (!nn_model)
  {
    cerr << "Get EvalModel failed\n";
  }

  // Load model with desired outputs
  std::string networkConfiguration;
  // with the ones specified.
  //networkConfiguration += "outputNodeNames=\"h1.z:ol.z\"\n";
  networkConfiguration += "deviceId=";
  networkConfiguration += to_string(device_id);
  networkConfiguration += "\n";
  networkConfiguration += "lockGPU=false\n";
  networkConfiguration += "modelPath=\"";
  networkConfiguration += uct_params_path;
  networkConfiguration += "/model2.bin\"";
  nn_model->CreateNetwork(networkConfiguration);

#if 0
  std::map<std::wstring, size_t> inDims;
  std::map<std::wstring, size_t> outDims;

  nn_model->GetNodeDimensions(inDims, Microsoft::MSR::CNTK::NodeGroup::nodeInput);
  nn_model->GetNodeDimensions(outDims, Microsoft::MSR::CNTK::NodeGroup::nodeOutput);
  cerr << "Input" << endl;
  for (auto p : inDims) {
    wcerr << p.first << ":" << p.second << endl;
  }
  cerr << "Output" << endl;
  for (auto p : outDims) {
    wcerr << p.first << ":" << p.second << endl;
  }
#endif

  cerr << "ok" << endl;
}

void
EvalPolicy(const std::vector<std::shared_ptr<policy_eval_req>>& requests,
  std::vector<float>& data_basic, std::vector<float>& data_features, std::vector<float>& data_history,
  std::vector<float>& data_color, std::vector<float>& data_komi)
{
  Layer inputLayer;
  inputLayer.insert(MapEntry(L"basic", &data_basic));
  inputLayer.insert(MapEntry(L"features", &data_features));
  inputLayer.insert(MapEntry(L"history", &data_history));
  inputLayer.insert(MapEntry(L"color", &data_color));
  inputLayer.insert(MapEntry(L"komi", &data_komi));
  Layer outputLayer;
  //std::vector<float> ownern;
  std::vector<float> moves;
  //ownern.reserve(pure_board_max * indices.size());
  moves.reserve(pure_board_max * requests.size());
  //outputLayer.insert(MapEntry(L"owner", &ownern));
  outputLayer.insert(MapEntry(L"ol", &moves));

  nn_model->Evaluate(inputLayer, outputLayer);

  if (moves.size() != pure_board_max * requests.size()) {
    cerr << "Eval move error " << moves.size() << endl;
    return;
  }
  //if (ownern.size() != pure_board_max * indices.size()) {
  //  cerr << "Eval owner error " << ownern.size() << endl;
  //  return;
  //}
  //cerr << "Eval " << indices.size() << " " << path.size() << endl;
  for (int j = 0; j < requests.size(); j++) {
    const auto req = requests[j];
    const int index = req->index;
    const int child_num = uct_node[index].child_num;
    child_node_t *uct_child = uct_node[index].child;
    const int ofs = pure_board_max * j;

    float sum = 0;
    for (int i = 0; i < pure_board_max; i++) {
      moves[i + ofs] = exp(moves[i + ofs]);
      sum += moves[i + ofs];
    }
    LOCK_NODE(index);

    int depth = req->depth;
#if 0
    if (index == current_root) {
      for (int i = 0; i < pure_board_max; i++) {
	int x = i % pure_board_size;
	int y = i / pure_board_size;
	owner_nn[POS(x + OB_SIZE, y + OB_SIZE)] = ownern[i + ofs];
      }
    }
#endif

#if 1
    bool flat = depth <= 2 && child_num > 3;
    vector<int> cs;
    for (int i = 1; i < child_num; i++) {
      int pos = RevTransformMove(uct_child[i].pos, req->trans);

      int x = X(pos) - OB_SIZE;
      int y = Y(pos) - OB_SIZE;
      int n = x + y * pure_board_size;
      double score = moves[n + ofs] / sum;
      //if (depth == 1) cerr << "RAW POLICY " << uct_child[i].pos << " " << req->trans << " " << FormatMove(pos) << " " << x << "," << y << " " << ofs << " -> " << score << endl;
      if (uct_child[i].ladder) {
	score /= 100;
      }
      /*if (uct_child[i].rate < 0.0) {
	uct_child[i].nnrate = uct_child[i].rate;
      }
      else */{
	//if (score > 0)
	//uct_child[i].flag = true;
	uct_child[i].nnrate = max(score, 0.0);

	if (flat) {
	   cs.push_back(i);
	}
      }
    }
    if (flat && cs.size() >= 3) {
       sort(cs.begin(), cs.end(),
	  [&](int a, int b) {
	  return uct_child[a].nnrate > uct_child[b].nnrate;
       });
       const int n = depth < 2 ? 3 : 2;
       double topsum = 0;
       for (int i = 0; i < n; i++) {
	  //cerr << "FLAT" << depth << " " << i << ":" << uct_child[cs[i]].nnrate << endl;
	  topsum += uct_child[cs[i]].nnrate;
       }

       for (int i = 0; i < n; i++) {
	  double org = uct_child[cs[i]].nnrate;
	  uct_child[cs[i]].nnrate = (org + topsum / n) / 2;
	  //cerr << "FLAT" << depth << " " << i << ":" << org << " -> " << uct_child[cs[i]].nnrate << endl;
       }
    }
    uct_node[index].evaled = true;
#endif
    UNLOCK_NODE(index);
  }
  eval_count_policy += requests.size();
}


void
EvalValue(const std::vector<std::shared_ptr<value_eval_req>>& requests,
  std::vector<float>& data_basic, std::vector<float>& data_features, std::vector<float>& data_history,
  std::vector<float>& data_color, std::vector<float>& data_komi)
{
  Layer inputLayer;
  inputLayer.insert(MapEntry(L"basic", &data_basic));
  inputLayer.insert(MapEntry(L"features", &data_features));
  inputLayer.insert(MapEntry(L"history", &data_history));
  inputLayer.insert(MapEntry(L"color", &data_color));
  inputLayer.insert(MapEntry(L"komi", &data_komi));
  Layer outputLayer;
  std::vector<float> win;
  win.reserve(requests.size());
  outputLayer.insert(MapEntry(L"p", &win));

  try {
    nn_model->Evaluate(inputLayer, outputLayer);
  } catch (const std::exception& err) {
    fprintf(stderr, "Evaluation failed. EXCEPTION occurred: %s\n", err.what());
    abort();
  } catch (...) {
    fprintf(stderr, "Evaluation failed. Unknown ERROR occurred.\n");
    abort();
  }

  if (win.size() != requests.size()) {
    cerr << "Eval win error " << win.size() << endl;
    return;
  }
  //cerr << "Eval " << indices.size() << " " << path.size() << endl;
  for (int j = 0; j < requests.size(); j++) {
    auto req = requests[j];

    double p = ((double)win[j] + 1) / 2;
    if (p < 0)
      p = 0;
    if (p > 1)
      p = 1;
    //cerr << "#" << index << "  " << sum << endl;

    double value = 1 - p;// color[j] == S_BLACK ? p : 1 - p;

    req->uct_child->value = value;
    for (int i = req->path.size() - 1; i >= 0; i--) {
      int current = req->path[i];
      if (current < 0)
	break;

      atomic_fetch_add(&uct_node[current].value_move_count, 1);
      atomic_fetch_add(&uct_node[current].value_win, value);
      value = 1 - value;
    }
  }
  eval_count_value += requests.size();
}

static std::vector<float> eval_input_data_basic;
static std::vector<float> eval_input_data_features;
static std::vector<float> eval_input_data_history;
static std::vector<float> eval_input_data_color;
static std::vector<float> eval_input_data_komi;

void EvalNode() {
#if 1
  while (true) {
    bool running = handle[0] != nullptr;
    if (!running
      && ((!reuse_subtree && !ponder) || (eval_policy_queue.empty() && eval_value_queue.empty()))) {
      break;
    }

    if (eval_policy_queue.empty() && eval_value_queue.empty()) {
      this_thread::sleep_for(chrono::milliseconds(1));
      //cerr << "EMPTY QUEUE" << endl;
      continue;
    }

    if (eval_policy_queue.size() == 0) {
    } else {
      std::vector<std::shared_ptr<policy_eval_req>> requests;

      LOCK_EXPAND;
      for (int i = 0; i < policy_batch_size && !eval_policy_queue.empty(); i++) {
	auto req = eval_policy_queue.front();
	requests.push_back(req);
	eval_policy_queue.pop();
      }
      UNLOCK_EXPAND;

      eval_input_data_basic.resize(0);
      eval_input_data_features.resize(0);
      eval_input_data_history.resize(0);
      eval_input_data_color.resize(0);
      eval_input_data_komi.resize(0);
      for (auto& req : requests) {
        std::copy(req->data_basic.begin(), req->data_basic.end(), std::back_inserter(eval_input_data_basic));
        std::copy(req->data_features.begin(), req->data_features.end(), std::back_inserter(eval_input_data_features));
        std::copy(req->data_history.begin(), req->data_history.end(), std::back_inserter(eval_input_data_history));
        eval_input_data_color.push_back(req->color - 1);
        eval_input_data_komi.push_back(komi[0]);
      }
      EvalPolicy(requests, eval_input_data_basic, eval_input_data_features, eval_input_data_history, eval_input_data_color, eval_input_data_komi);
    }

    if (running && eval_value_queue.size() == 0) {
    } else {
      std::vector<std::shared_ptr<value_eval_req>> requests;

      LOCK_EXPAND;
      for (int i = 0; i < value_batch_size && !eval_value_queue.empty(); i++) {
	auto req = eval_value_queue.front();
	requests.push_back(req);
	eval_value_queue.pop();
      }
      UNLOCK_EXPAND;

      eval_input_data_basic.resize(0);
      eval_input_data_features.resize(0);
      eval_input_data_history.resize(0);
      eval_input_data_color.resize(0);
      eval_input_data_komi.resize(0);
      for (auto& req : requests) {
        std::copy(req->data_basic.begin(), req->data_basic.end(), std::back_inserter(eval_input_data_basic));
        std::copy(req->data_features.begin(), req->data_features.end(), std::back_inserter(eval_input_data_features));
        std::copy(req->data_history.begin(), req->data_history.end(), std::back_inserter(eval_input_data_history));
        eval_input_data_color.push_back(req->color - 1);
        eval_input_data_komi.push_back(komi[0]);
      }
      EvalValue(requests, eval_input_data_basic, eval_input_data_features, eval_input_data_history, eval_input_data_color, eval_input_data_komi);
    }
  }
#endif
}

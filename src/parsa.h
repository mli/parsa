#pragma once
#include "system/app.h"
#include "data/proto/example.pb.h"
#include "util/producer_consumer.h"
#include "data/stream_reader.h"
#include "util/sparse_matrix.h"
#include "util/bitmap.h"
#include "util/recordio.h"
#include "parameter/kv_map.h"
#include "parameter/kv_vector.h"
#include "double_linked_array.h"
#include "parsa.pb.h"

using namespace PS;

static const int kMaxNumPartitions = 16;
typedef uint8 P;  // support up to 256 partitions
typedef uint16 V; // support up to 16 partitions

typedef std::pair<Key, P> KP;

inline Call getCall(const MessagePtr& msg) {
  return msg->task.GetExtension(parsa);
}

inline Task newTask(Call::Command cmd) {
  Task task; task.MutableExtension(parsa)->set_cmd(cmd);
  return task;
}

class Parsa : public App {
 public:
  Parsa(const string& conf) : App() {
    CHECK(google::protobuf::TextFormat::ParseFromString(conf, &conf_));
  }
  virtual ~Parsa() { }
 protected:
  Config conf_;
};

class ParsaScheduler : public Parsa {
 public:
  ParsaScheduler(const string& conf) : Parsa(conf) { }
  virtual ~ParsaScheduler() { }
  void run();
};

class ParsaServerModel : public KVMap<Key, V> {
 public:
  ParsaServerModel(const string& name) : KVMap<Key, V>(name) { }
  virtual ~ParsaServerModel() { }
  void setConf(const Config& conf) { conf_ = conf; }
  virtual void setValue(const MessagePtr& msg);
  void partitionV();
 private:
  Config conf_;
  bool enter_real_stage1_ = false;
  int count_ = 0;
  std::unordered_map<NodeID, SArray<Key>> worker_key_;
};

class ParsaServer : public Parsa {
 public:
  ParsaServer(const string& conf) : Parsa(conf) {
    model_ = new ParsaServerModel("model");
    model_->setConf(conf_);
  }
  virtual ~ParsaServer() { }

  void process(const MessagePtr& msg) {
    if (getCall(msg).cmd() == Call::PARTITION_V) {
      model_->partitionV();
    }
  }
 private:
  ParsaServerModel* model_;
};

typedef float Empty;
typedef SparseMatrix<uint32, Empty> Graph;
typedef std::shared_ptr<Graph> GraphPtr;
typedef std::vector<GraphPtr> GraphPtrList;
typedef std::vector<Example> ExampleList;
typedef std::shared_ptr<ExampleList> ExampleListPtr;

class ParsaWorker : public Parsa {
 public:
  ParsaWorker(const string& conf) : Parsa(conf) { }
  virtual ~ParsaWorker() { }

  void init();
  void process(const MessagePtr& msg);
 private:
  struct BlockData {
    GraphPtr row_major;
    GraphPtr col_major;
    // SArray<Key> global_key;
    ExampleListPtr examples;
    int pull_time;
    int blk_id;
  };

  void readGraph(
      StreamReader<Empty>& stream, ProducerConsumer<BlockData>& producer,
      int& start_id, int end_id, int block_size, bool keep_examples);

  void stage0();
  void stage1();
  void remapKey();

  void partitionU(const BlockData& blk, SArray<int>* map_U);
  void initCost(const GraphPtr& row_major_blk);
  void updateCostAndNeighborSet(
    const GraphPtr& row_major_blk, const GraphPtr& col_major_blk,
    const SArray<Key>& global_key, int Ui, int partition);
  void initNeighborSet(const SArray<V>& nbset);
  void sendUpdatedNeighborSet(int blk);

 private:
  DataConfig input_graph_;
  std::vector<Bitmap> neighbor_set_;

  SArray<KP> added_neighbor_set_;
  SArray<Key> added_nbset_key_;
  SArray<V> added_nbset_value_;
  bool delta_nbset_;

  // about U
  std::vector<DblinkArray> cost_;
  Bitmap assigned_U_;

  KVVector<Key, V>* sync_nbset_;
  bool no_sync_;

  std::vector<int> push_time_;
  int num_partitions_;
  bool random_partition_;

  DataConfig tmp_files_;
};

#include "parsa.h"
#include "util/localizer.h"
#include "system/postmaster.h"

void ParsaScheduler::run() {
  // partition data
  Task data = newTask(Call::ASSIGN_DATA);
  auto conf = Postmaster::partitionData(conf_.input_graph(), sys_.yp().num_workers());
  std::vector<Task> tasks(conf.size(), data);
  for (int i = 0; i < conf.size(); ++i) {
    conf[i].set_ignore_feature_group(true);
    *tasks[i].MutableExtension(parsa)->mutable_data() = conf[i];
  }
  port(kWorkerGroup)->submitAndWait(tasks);

  if (conf_.stage0_blocks()) {
    Task partitionU = newTask(Call::PARTITION_U_STAGE_0);
    port("W0")->submitAndWait(partitionU);
  }

  if (conf_.stage1_block_size()) {
    Task partitionU = newTask(Call::PARTITION_U_STAGE_1);
    port(kCompGroup)->submitAndWait(partitionU);
  }

  Task partitionV = newTask(Call::PARTITION_V);
  port(kCompGroup)->submitAndWait(partitionV);
}

void ParsaServerModel::setValue(const MessagePtr& msg) {
  // clear the neigbhor set in the initialization stage
  ++ count_;
  int stage1_real_work = conf_.stage1_warm_up_blocks() +
                         conf_.stage0_warm_up_blocks() +
                         conf_.stage0_blocks();
  int chn = msg->task.key_channel();
  if (chn == stage1_real_work && !enter_real_stage1_) {
    enter_real_stage1_ = true;
    data_.clear();
  } else if (chn < stage1_real_work &&
             count_ % conf_.clear_nbset_per_num_blocks() == 0) {
    data_.clear();
  }

  if (chn >= stage1_real_work && conf_.save_result()) {
    // store the key for workers
    SArray<Key> recv_key(msg->key);
    worker_key_[msg->sender] = recv_key.setUnion(worker_key_[msg->sender]);
  }

  KVMap<Key, V>::setValue(msg);
}

void ParsaServerModel::partitionV() {
  int num_partitions = conf_.num_partitions();
  bool random_partition = conf_.randomly_partition_v();
  std::vector<int> cost(num_partitions);
  srand(time(NULL));

  int n = data_.size();
  SArray<KP> partition; partition.reserve(n);
  for (const auto& e : data_) {
    if (!e.second) continue;
    if (random_partition) {
      P best_k = rand() % num_partitions;
      for (int k = 0; k < num_partitions; ++k) {
        if ((e.second & (1 << k)) && k != best_k) {
          ++ cost[k];
        }
      }
    } else {
      // greedy
      int max_cost = 0;
      P best_k = -1;
      for (P k = 0; k < num_partitions; ++k) {
        if (e.second & (1 << k)) {
          int c = ++ cost[k];
          if (c > max_cost) { max_cost = c; best_k = k; }
        }
      }
      CHECK_GE(best_k, 0);
      partition.pushBack(std::make_pair(e.first, best_k));
      -- cost[best_k];
    }
  }

  int v = 0;
  for (int j = 0; j < num_partitions; ++j) v += cost[j];
  LL << "V size: " << data_.size() << " cost: " << v;
  data_.clear();

  if (!conf_.save_result()) return;
  // push the results to workers
  std::sort(partition.begin(), partition.end(),
            [](const KP& a, const KP& b) { return a.first < b.first; });
  SArray<Key> V_key(n);
  SArray<V> V_val(n);
  for (int i = 0; i < n; ++i) {
    V_key[i] = partition[i].first;
    V_val[i] = partition[i].second;
  }
  partition.clear();


  int chn = conf_.stage0_warm_up_blocks() + conf_.stage0_blocks() +
            conf_.stage1_warm_up_blocks() + conf_.stage1_blocks();
  for (const auto& it : worker_key_) {
    MessagePtr V_msg(new Message(it.first, chn*3));
    V_msg->task.set_key_channel(chn);
    V_msg->setKey(it.second);
    SArray<V> val;
    parallelOrderedMatch(V_key, V_val, it.second, &val);
    V_msg->addValue(val);
    this->set(V_msg)->set_gather(true);
    this->push(V_msg);
  }
}

void ParsaWorker::init() {
  conf_.mutable_input_graph()->set_ignore_feature_group(true);
  num_partitions_ = conf_.num_partitions();
  neighbor_set_.resize(num_partitions_);
  random_partition_ = conf_.randomly_partition_u();
  sync_nbset_ = new KVVector<Key, V>("model");
}

void ParsaWorker::process(const MessagePtr& msg) {
  auto cmd = getCall(msg).cmd();
  if (cmd == Call::PARTITION_U_STAGE_0) {
    stage0();
  } else if (cmd == Call::PARTITION_U_STAGE_1) {
    stage1();
  } else if (cmd == Call::PARTITION_V) {
    remapKey();
  } else if (cmd == Call::ASSIGN_DATA) {
    input_graph_ = getCall(msg).data();
  }
}

void ParsaWorker::readGraph(
    StreamReader<Empty>& reader, ProducerConsumer<BlockData>& producer,
    int& start_id, int end_id, int block_size, bool keep_examples) {
  CHECK_GT(end_id, start_id);
  producer.startProducer(
      [this, &reader, &start_id, end_id, block_size, keep_examples] (
          BlockData* blk, size_t* size)->bool {
        // read a block
        MatrixPtrList<Empty> X;
        auto examples = ExampleListPtr(new ExampleList());
        bool ret = reader.readMatrices(
            block_size, &X, keep_examples ? examples.get() : nullptr);
        if (X.empty()) return false;

        // find the unique keys
        auto G = std::static_pointer_cast<SparseMatrix<Key,Empty>>(X.back());
        G->info().set_type(MatrixInfo::SPARSE_BINARY);
        G->value().clear();
        Localizer<Key, Empty> localizer;
        SArray<Key> key;
        localizer.countUniqIndex(G, &key);

        // pull the current partition from servers
        sync_nbset_->key(start_id) = key;
        int time = -1;
        if (!no_sync_) {
          MessagePtr pull(new Message(kServerGroup));
          pull->task.set_key_channel(start_id);
          pull->setKey(key);
          pull->addFilter(FilterConfig::KEY_CACHING);
          time = sync_nbset_->pull(pull);
        }

        // preprocess data and store the results
        blk->row_major  = std::static_pointer_cast<Graph>(localizer.remapIndex(key));
        blk->col_major  = std::static_pointer_cast<Graph>(blk->row_major->toColMajor());
        blk->examples   = examples;
        blk->pull_time  = time;
        blk->blk_id     = start_id ++;
        *size           = 1;  // a fake number, i'm lazy to get the correct one

        // LL << start_id << "  " << end_id;
        return start_id == end_id ? false : ret;
      });
}


void ParsaWorker::stage0() {
  no_sync_ = true;
  delta_nbset_ = false;

  // warm up
  if (conf_.stage0_warm_up_blocks()) {
    StreamReader<Empty> stream_0(input_graph_);
    ProducerConsumer<BlockData> reader_0(conf_.data_buff_size_in_mb());
    int start_id_0 = 0;
    int end_id_0 = start_id_0 + conf_.stage0_warm_up_blocks();
    readGraph(stream_0, reader_0, start_id_0, end_id_0, conf_.stage0_block_size(), false);

    BlockData blk;
    while (reader_0.pop(&blk)) {
      auto& value = sync_nbset_->value(blk.blk_id);
      parallelOrderedMatch(
          added_nbset_key_, added_nbset_value_, sync_nbset_->key(blk.blk_id), &value);
      partitionU(blk, nullptr);
    }
    LL << "stage 0: initialized by " << start_id_0 << " blocks";
  }

  // real work
  if (conf_.stage0_blocks()) {
    StreamReader<Empty> stream_1(input_graph_);
    ProducerConsumer<BlockData> reader_1(conf_.data_buff_size_in_mb());
    int start_id_1 = conf_.stage0_warm_up_blocks();
    int start_id_1_const = start_id_1;
    int end_id_1 = start_id_1 + conf_.stage0_blocks();
    readGraph(stream_1, reader_1, start_id_1, end_id_1, conf_.stage0_block_size(), false);

    BlockData blk;
    SArray<Key> nbset_key;
    SArray<V> nbset_value;
    while (reader_1.pop(&blk)) {
      auto& value = sync_nbset_->value(blk.blk_id);
      if (blk.blk_id == start_id_1_const) {
        // the first block, use the nbset of the last the block
        parallelOrderedMatch<Key, V, OpOr<V>>(
            added_nbset_key_, added_nbset_value_, sync_nbset_->key(blk.blk_id), &value);
      } else {
        // do not throw away nbset now
        SArray<Key> tmp_key;
        SArray<V> tmp_value;
        parallelUnion<Key, V, OpOr<V>>(
            nbset_key, nbset_value, added_nbset_key_, added_nbset_value_,
            &tmp_key, &tmp_value);
        nbset_key = tmp_key;
        nbset_value = tmp_value;

        parallelOrderedMatch<Key, V, OpOr<V>>(
            nbset_key, nbset_value, sync_nbset_->key(blk.blk_id), &value);

        delta_nbset_ = true;
      }

      partitionU(blk, nullptr);
    }

    MessagePtr push(new Message(kServerGroup));
    push->task.set_key_channel(end_id_1);
    push->setKey(nbset_key);
    push->addValue(nbset_value);
    push->wait = true;
    sync_nbset_->set(push)->set_op(Operator::OR);
    sync_nbset_->push(push);
    LL << "stage 0: partitioned " << start_id_1 - start_id_1_const << " blocks";
  }

}


void ParsaWorker::stage1() {
  no_sync_ = false;
  delta_nbset_ = false;

  int start_id_0_const = conf_.stage0_warm_up_blocks() + conf_.stage0_blocks();
  // warm up
  if (conf_.stage1_warm_up_blocks()) {
    StreamReader<Empty> stream_0(input_graph_);
    ProducerConsumer<BlockData> reader_0(conf_.data_buff_size_in_mb());
    int start_id_0 = start_id_0_const;
    int end_id_0 = start_id_0 + conf_.stage1_warm_up_blocks();
    readGraph(stream_0, reader_0, start_id_0, end_id_0, conf_.stage1_block_size(), false);

    BlockData blk;
    while (reader_0.pop(&blk)) {
      partitionU(blk, nullptr);
    }

    for (int t : push_time_) sync_nbset_->waitOutMsg(kServerGroup, t);
    push_time_.clear();

    LL << "stage 1: initialized by " << start_id_0 - start_id_0_const << " blocks";
  }

  // real work
  // reader
  StreamReader<Empty> stream_1(input_graph_);
  ProducerConsumer<BlockData> reader_1(conf_.data_buff_size_in_mb());
  int start_id_1 = conf_.stage1_warm_up_blocks() + start_id_0_const;
  int start_id_1_const = start_id_1;
  int end_id_1 = start_id_1 + conf_.stage1_blocks();
  readGraph(stream_1, reader_1, start_id_1, end_id_1, conf_.stage1_block_size(), true);

  // write the partitioned examples into protobuf format
  typedef std::pair<ExampleListPtr, SArray<int>> ResultPair;
  ProducerConsumer<ResultPair> writer_1;
  std::vector<RecordWriter> proto_writers_1;
  proto_writers_1.resize(num_partitions_);
  tmp_files_.set_ignore_feature_group(true);
  tmp_files_.set_format(DataConfig::PROTO);
  for (int i = 0; i < num_partitions_; ++i) {
    char prefix[100]; snprintf(prefix, 100, "_%s_%03d", myNodeID().c_str(), i);
    tmp_files_.add_file(conf_.output_graph().file(0) + string(prefix));
    auto file = File::openOrDie(ithFile(tmp_files_, i), "w");
    proto_writers_1[i] = RecordWriter(file);
  }
  writer_1.setCapacity(conf_.data_buff_size_in_mb());
  writer_1.startConsumer([&proto_writers_1](const ResultPair& data) {
      const auto& examples = *data.first;
      const auto& partition = data.second;
      CHECK_EQ(examples.size(), partition.size());
      for (int i = 0; i < examples.size(); ++i) {
        CHECK(proto_writers_1[partition[i]].WriteProtocolMessage(examples[i]));
      }
    });

  // partition U
  // int y = 0;
  BlockData blk;
  SArray<int> map_U;
  while (reader_1.pop(&blk)) {
    partitionU(blk, &map_U);
    writer_1.push(std::make_pair(blk.examples, map_U));
  }
  writer_1.setFinished();
  writer_1.waitConsumer();

  for (int t : push_time_) sync_nbset_->waitOutMsg(kServerGroup, t);
  push_time_.clear();
  LL << "stage 1: partitioned " << start_id_1 - start_id_1_const << " blocks";
}

void ParsaWorker::remapKey() {
  if (!conf_.save_result()) return;
  // wait the partition results from servers
  int chn = conf_.stage0_warm_up_blocks() + conf_.stage0_blocks() +
            conf_.stage1_warm_up_blocks() + conf_.stage1_blocks();
  sync_nbset_->waitInMsg(kServerGroup, chn*3);
  // LL << sync_nbset_->key(chn);
  // LL << sync_nbset_->value(chn);

  // construct the map
  auto key = sync_nbset_->key(chn);
  auto val = sync_nbset_->value(chn);
  int n =  key.size();
  SArray<KP> data(n);
  for (int i = 0; i < n; ++i) {
    data[i] = make_pair(key[i], val[i]);
  }
  key.clear();
  val.clear();
  std::unordered_map<Key, P> map;
  map.insert(data.begin(), data.end());

  // remap the data
  // bool validate = conf_.validate();
  // SArray<Key> remote_keys;

  for (int i = 0; i < tmp_files_.file_size(); ++i) {
    RecordReader reader(File::open(ithFile(tmp_files_, i), "r"));
    RecordWriter writer(File::open(ithFile(tmp_files_, i, "_recordio"), "w"));
    Example ex;
    uint64 itv = kMaxKey / num_partitions_;
    while (reader.ReadProtocolMessage(&ex)) {
      for (int i = 0; i < ex.slot_size(); ++i) {
        auto mut = ex.mutable_slot(i);
        if (mut->id() == 0) continue;
        for (int j = 0; j < mut->key_size(); ++j) {
          auto key = mut->key(j);
          auto p = map[key];
          auto new_key = key / (key < itv ? 1 : num_partitions_) + p * itv;
          mut->set_key(j, new_key);
          // if (validate && p != i) remote_keys.pushBack(new_key);
        }
      }
      writer.WriteProtocolMessage(ex);
    }
    File::remove(tmp_files_.file(i));
  }

  // if (validate) {
  //   std::sort(remote_keys.begin(), remote_keys.end());
  //   auto it = std::unique(remote_keys.begin(), remote_keys.end());
  //   LL << it - remote_keys.begin();
  // }
}


void ParsaWorker::partitionU(const BlockData& blk, SArray<int>* map_U) {
  if (blk.pull_time >= 0) {
    sync_nbset_->waitOutMsg(kServerGroup, blk.pull_time);
  }
  int id = blk.blk_id;
  initNeighborSet(sync_nbset_->value(id));

  int n = blk.row_major->rows();
  if (map_U) map_U->resize(n);
  assigned_U_.clear();
  assigned_U_.resize(n);
  initCost(blk.row_major);

  std::vector<int> rnd_idx;
  if (random_partition_) {
    rnd_idx.resize(n);
    for (int i = 0; i < n; ++i) rnd_idx[i] = i;
    srand(time(NULL));
    random_shuffle(rnd_idx.begin(), rnd_idx.end());
  }

  // partitioning

  auto key = sync_nbset_->key(id);
  for (int i = 0; i < n; ++i) {
    // assing U_i to partition k
    int k = i % num_partitions_;
    int Ui = random_partition_ ? rnd_idx[i] : cost_[k].minIdx();
    assigned_U_.set(Ui);
    if (map_U) (*map_U)[Ui] = k;

    // update
    updateCostAndNeighborSet(blk.row_major, blk.col_major, key, Ui, k);
  }

  // send results to servers
  sendUpdatedNeighborSet(id);
  sync_nbset_->clear(id);
}

void ParsaWorker::initNeighborSet(const SArray<V>& nbset) {
  int n = nbset.size();
  for (int i = 0; i < num_partitions_; ++i) {
    neighbor_set_[i].clear();
    neighbor_set_[i].resize(n, false);
  }

  for (int i = 0; i < n; ++i) {
    V s = nbset[i];
    if (s == 0) continue;
    for (int k = 0; k < num_partitions_; ++k) {
      if (s & (1 << k)) neighbor_set_[k].set(i);
    }
  }
  added_neighbor_set_.resize(0);
}

void ParsaWorker::sendUpdatedNeighborSet(int blk_id) {
  auto& nbset = added_neighbor_set_;
  if (nbset.empty()) return;

  // pack the local updates
  std::sort(nbset.begin(), nbset.end(),
            [](const KP& a, const KP& b) { return a.first < b.first; });

  added_nbset_key_.resize(0);
  added_nbset_value_.resize(0);
  Key pre = nbset[0].first;
  V s = 0;
  for (int i = 0; i < nbset.size(); ++i) {
    Key cur = nbset[i].first;
    if (cur != pre) {
      added_nbset_value_.pushBack(s);
      added_nbset_key_.pushBack(pre);
      pre = cur;
      s = 0;
    }
    s |= 1 << nbset[i].second;
  }
  added_nbset_key_.pushBack(nbset.back().first);
  added_nbset_value_.pushBack(s);

  // send local updates
  if (!no_sync_) {
    MessagePtr push(new Message(kServerGroup));
    push->task.set_key_channel(blk_id);
    push->setKey(added_nbset_key_);
    push->addValue(added_nbset_value_);
    push->addFilter(FilterConfig::KEY_CACHING)->set_clear_cache_if_done(true);
    sync_nbset_->set(push)->set_op(Operator::OR);
    push_time_.push_back(sync_nbset_->push(push));
  }
}

// init the cost of assigning U_i to partition k
void ParsaWorker::initCost(const GraphPtr& row_major_blk) {
  cost_.resize(num_partitions_);
  if (random_partition_) return;
  int n = row_major_blk->rows();
  size_t* row_os = row_major_blk->offset().begin();
  uint32* row_idx = row_major_blk->index().begin();
  for (int k = 0; k < num_partitions_; ++k) {
    std::vector<int> cost(n);
    const auto& assigned_V = neighbor_set_[k];
    for (int i = 0; i < n; ++ i) {
      for (size_t j = row_os[i]; j < row_os[i+1]; ++j) {
        cost[i] += !assigned_V.test(row_idx[j]);
      }
    }
    cost_[k].init(cost, conf_.max_cached_cost_value());
  }
}

void ParsaWorker::updateCostAndNeighborSet(
    const GraphPtr& row_major_blk, const GraphPtr& col_major_blk,
    const SArray<Key>& global_key, int Ui, int partition) {
  if (!random_partition_) {
    for (int s = 0; s < num_partitions_; ++s) cost_[s].remove(Ui);
    if (cost_[partition][Ui] == 0) return;
  }

  size_t* row_os = row_major_blk->offset().begin();
  size_t* col_os = col_major_blk->offset().begin();
  uint32* row_idx = row_major_blk->index().begin();
  uint32* col_idx = col_major_blk->index().begin();
  auto& assigned_V = neighbor_set_[partition];
  auto& cost = cost_[partition];
  for (size_t i = row_os[Ui]; i < row_os[Ui+1]; ++i) {
    int Vj = row_idx[i];
    bool assigned = assigned_V.test(Vj);
    if (!(delta_nbset_ && assigned)) {
      added_neighbor_set_.pushBack(std::make_pair(global_key[Vj], (P)partition));
    }
    if (assigned) continue;
    assigned_V.set(Vj);
    for (size_t s = col_os[Vj]; s < col_os[Vj+1]; ++s) {
      int Uk = col_idx[s];
      if (assigned_U_[Uk]) continue;
      if (!random_partition_) cost.decrAndSort(Uk);
    }
  }
}

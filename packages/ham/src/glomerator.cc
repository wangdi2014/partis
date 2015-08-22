#include "glomerator.h"

namespace ham {


// ----------------------------------------------------------------------------------------
string SeqStr(vector<Sequence> &seqs, string delimiter) {
  string seq_str;
  for(size_t iseq = 0; iseq < seqs.size(); ++iseq) {
    if(iseq > 0) seq_str += delimiter;
    seq_str += seqs[iseq].undigitized();
  }
  return seq_str;
}

// ----------------------------------------------------------------------------------------
string SeqNameStr(vector<Sequence> &seqs, string delimiter) {
  string name_str;
  for(size_t iseq = 0; iseq < seqs.size(); ++iseq) {
    if(iseq > 0) name_str += delimiter;
    name_str += seqs[iseq].name();
  }
  return name_str;
}

// ----------------------------------------------------------------------------------------
Glomerator::Glomerator(HMMHolder &hmms, GermLines &gl, vector<vector<Sequence> > &qry_seq_list, Args *args, Track *track) :
  track_(track),
  args_(args),
  vtb_dph_("viterbi", args_, gl, hmms),
  fwd_dph_("forward", args_, gl, hmms),
  i_initial_partition_(0),
  n_fwd_cached_(0),
  n_fwd_calculated_(0),
  n_vtb_cached_(0),
  n_vtb_calculated_(0),
  n_hfrac_calculated_(0),
  n_hamming_merged_(0)
{
  ReadCachedLogProbs();

  Partition tmp_partition;
  int last_ipath(0);
  assert(args_->integers_["path_index"].size() == qry_seq_list.size());
  for(size_t iqry = 0; iqry < qry_seq_list.size(); iqry++) {
    string key = SeqNameStr(qry_seq_list[iqry], ":");

    int ipath(args_->integers_["path_index"][iqry]);
    if(last_ipath != ipath) {  // if we need to push back a new initial partition (i.e. this is a new path/particle) NOTE I'm assuming the the first one will have <path_index> zero
      initial_partitions_.push_back(tmp_partition);
      initial_logprobs_.push_back(LogProbOfPartition(tmp_partition));
      initial_logweights_.push_back(args_->floats_["logweight"][iqry]);  // it's the same for each <iqry> with this <path_index>
      tmp_partition.clear();
      last_ipath = ipath;
    }
      
    tmp_partition.insert(key);

    if(seq_info_.count(key) > 0)  // already added this cluster to the maps (but it'll appear more than once if it's in multiple paths/particles), so we only need to add the cluster info to <initial_partitions_>
      continue;

    seq_info_[key] = qry_seq_list[iqry];
    only_genes_[key] = args_->str_lists_["only_genes"][iqry];

    KSet kmin(args_->integers_["k_v_min"][iqry], args_->integers_["k_d_min"][iqry]);
    KSet kmax(args_->integers_["k_v_max"][iqry], args_->integers_["k_d_max"][iqry]);
    KBounds kb(kmin, kmax);
    kbinfo_[key] = kb;

    vector<KBounds> kbvector(seq_info_[key].size(), kbinfo_[key]);

    mute_freqs_[key] = avgVector(args_->float_lists_["mute_freqs"][iqry]);
  }
  // add the last initial partition (i.e. for the last path/particle)
  assert(tmp_partition.size() > 0);
  initial_partitions_.push_back(tmp_partition);
  initial_logprobs_.push_back(LogProbOfPartition(tmp_partition));
  initial_logweights_.push_back(args_->floats_["logweight"].back());

  // the *first* time, we only get one path/partition from partis, so we push back a bunch of copies
  if((int)initial_partitions_.size() == 1 && args_->smc_particles() > 1)  {
    for(int ip=1; ip<args_->smc_particles(); ++ip) {
      initial_partitions_.push_back(tmp_partition);
      initial_logprobs_.push_back(LogProbOfPartition(tmp_partition));
      initial_logweights_.push_back(args_->floats_["logweight"].back());
    }
  }

  if((int)initial_partitions_.size() != args_->smc_particles())
    throw runtime_error("wrong number of initial partitions " + to_string(initial_partitions_.size()) + " (should be " + to_string(args_->smc_particles()) + ")");

  if(args_->debug())
    for(auto &part : initial_partitions_)
      PrintPartition(part, "initial");
}

// ----------------------------------------------------------------------------------------
Glomerator::~Glomerator() {
  // cout << "        cached vtb " << n_vtb_cached_ << "/" << (n_vtb_cached_ + n_vtb_calculated_)
  //      << "    fwd " << n_fwd_cached_ << "/" << (n_fwd_cached_ + n_fwd_calculated_) << endl;
  cout << "        calculated   vtb " << n_vtb_calculated_ << "    fwd " << n_fwd_calculated_ << "   hamming merged " << n_hamming_merged_
       << "    naive hfracs " << n_hfrac_calculated_
       << endl;
  if(args_->cachefile() != "")
    WriteCachedLogProbs();
}

// ----------------------------------------------------------------------------------------
void Glomerator::CacheNaiveSeqs() {  // they're written to file in the destructor, so we just need to calculate them here
  for(auto &kv : seq_info_)
    GetNaiveSeq(kv.first);
}

// ----------------------------------------------------------------------------------------
void Glomerator::Cluster() {
  if(args_->debug()) cout << "   glomerating" << endl;

  assert((int)initial_partitions_.size() == 1);
  ClusterPath cp(initial_partitions_[0], LogProbOfPartition(initial_partitions_[0]), initial_logweights_[0]);
  do {
    Merge(&cp);
  } while(!cp.finished_);

  vector<ClusterPath> paths{cp};
  WritePartitions(paths);
}

// ----------------------------------------------------------------------------------------
Partition Glomerator::GetAnInitialPartition(int &initial_path_index, double &logweight) {
  initial_path_index = i_initial_partition_;
  assert(i_initial_partition_ < (int)initial_partitions_.size());
  logweight = initial_logweights_[i_initial_partition_];
  return initial_partitions_.at(i_initial_partition_++);
}

// ----------------------------------------------------------------------------------------
void Glomerator::ReadCachedLogProbs() {
  ifstream ifs(args_->cachefile());
  if(!ifs.is_open()) {  // this means we don't have any cached results to start with, but we'll write out what we have at the end of the run to this file
    cout << "        cachefile d.n.e." << endl;
    return;
  }
  string line;
  // check the header is right (no cached info)
  if(!getline(ifs, line)) {
    cout << "        empty cachefile" << endl;
    return;  // return for zero length file
  }
  line.erase(remove(line.begin(), line.end(), '\r'), line.end());
  vector<string> headstrs(SplitString(line, ","));
  assert(headstrs[0].find("unique_ids") == 0);  // each set of unique_ids can appear many times, once for each truncation
  assert(headstrs[1].find("logprob") == 0);
  assert(headstrs[2].find("naive_seq") == 0);
  assert(headstrs[3].find("naive_hfrac") == 0);
  assert(headstrs[4].find("cyst_position") == 0);

  // NOTE there can be two lines with the same key (say if in one run we calculated the naive seq, and in a later run calculated the log prob)
  while(getline(ifs, line)) {
    line.erase(remove(line.begin(), line.end(), '\r'), line.end());
    vector<string> column_list = SplitString(line, ",");
    assert(column_list.size() == 6);
    string query(column_list[0]);

    string logprob_str(column_list[1]);
    if(logprob_str.size() > 0) {
      log_probs_[query] = stof(logprob_str);
      initial_log_probs_.insert(query);
    }

    string naive_seq(column_list[2]);

    string naive_hfrac_str(column_list[3]);
    if(naive_hfrac_str.size() > 0) {
      naive_hfracs_[query] = stof(naive_hfrac_str);
      initial_naive_hfracs_.insert(query);
    }

    int cyst_position(atoi(column_list[4].c_str()));

    if(naive_seq.size() > 0) {
      naive_seqs_[query] = Sequence(track_, query, naive_seq, cyst_position);
      initial_naive_seqs_.insert(query);
    }
  }
  cout << "        read " << log_probs_.size() << " cached logprobs and " << naive_seqs_.size() << " naive seqs" << endl;
}

// ----------------------------------------------------------------------------------------
double Glomerator::LogProbOfPartition(Partition &partition, bool debug) {
  if(args_->no_fwd())  // if we're doing pure naive hamming glomeration, we don't want to calculate any forward probs
    return -INFINITY;

  // get log prob of entire partition given by the keys in <partinfo> using the individual log probs in <log_probs>
  double total_log_prob(0.0);
  if(debug)
    cout << "LogProbOfPartition: " << endl;
  for(auto &key : partition) {
    // assert(SameLength(seq_info_[key], true));
    GetLogProb(key, seq_info_[key], kbinfo_[key], only_genes_[key], mute_freqs_[key]);  // immediately returns if we already have it NOTE all the sequences in <seq_info_[key]> are already the same length, since we've already merged them
    if(debug)
      cout << "  " << log_probs_[key] << "  " << key << endl;
    total_log_prob = AddWithMinusInfinities(total_log_prob, log_probs_[key]);
  }
  if(debug)
    cout << "  total: " << total_log_prob << endl;
  return total_log_prob;
}

// ----------------------------------------------------------------------------------------
void Glomerator::PrintPartition(Partition &partition, string extrastr) {
  const char *extra_cstr(extrastr.c_str());  // dammit I shouldn't need this line
  printf("    %-8.2f %s partition\n", LogProbOfPartition(partition), extra_cstr);
  for(auto &key : partition)
    cout << "          " << key << endl;
}

// ----------------------------------------------------------------------------------------
  // void Glomerator::WriteCacheLine(ofstream &ofs, string query, string logprob, string naive_seq, string naive_hfrac, string cpos, string errors) {
void Glomerator::WriteCacheLine(ofstream &ofs, string query) {
  ofs << query << ",";
  if(log_probs_.count(query))
    ofs << log_probs_[query];
  ofs << ",";
  if(naive_seqs_.count(query))
    ofs << naive_seqs_[query].undigitized();
  ofs << ",";
  if(!args_->dont_write_naive_hfracs() && naive_hfracs_.count(query))
    ofs << naive_hfracs_[query];
  ofs << ",";
  if(naive_seqs_.count(query))
    ofs << naive_seqs_[query].cyst_position();
  ofs << ",";
  if(errors_.count(query))
    ofs << errors_[query];
  ofs << endl;
}

// ----------------------------------------------------------------------------------------
void Glomerator::WriteCachedLogProbs() {
  ofstream log_prob_ofs(args_->cachefile());
  if(!log_prob_ofs.is_open())
    throw runtime_error("ERROR cache file (" + args_->cachefile() + ") d.n.e.\n");
  log_prob_ofs << "unique_ids,logprob,naive_seq,naive_hfrac,cyst_position,errors" << endl;

  log_prob_ofs << setprecision(20);
  for(auto &kv : log_probs_) {  // first write everything for which we have log probs
    if(!initial_log_probs_.count(kv.first))  // as long as we didn't have it to start with
      WriteCacheLine(log_prob_ofs, kv.first);
  }

  for(auto &kv : naive_seqs_) {  // then write the queries for which we have naive seqs but not logprobs
    if(log_probs_.count(kv.first) && !initial_log_probs_.count(kv.first))  // already wrote it
      continue;
    if(!initial_naive_seqs_.count(kv.first))
      WriteCacheLine(log_prob_ofs, kv.first);
  }

  if(!args_->dont_write_naive_hfracs()) {
    for(auto &kv : naive_hfracs_) {  // then write any queries for which we have naive hamming fractions, but no logprobs of naive seqs
      if(log_probs_.count(kv.first) && !initial_log_probs_.count(kv.first))
	continue;
      if(naive_seqs_.count(kv.first) && !initial_naive_seqs_.count(kv.first))
	continue;
      if(!initial_naive_hfracs_.count(kv.first))
	WriteCacheLine(log_prob_ofs, kv.first);
    }
  }

  log_prob_ofs.close();
}

// ----------------------------------------------------------------------------------------
void Glomerator::WritePartitions(vector<ClusterPath> &paths) {
  ofs_.open(args_->outfile());
  ofs_ << setprecision(20);
  ofs_ << "path_index,initial_path_index,partition,logprob,logweight" << endl;
  int ipath(0);
  for(auto &cp : paths) {
    for(unsigned ipart=0; ipart<cp.partitions().size(); ++ipart) {
      ofs_ << ipath << "," << cp.initial_path_index_ << ",";
      int ic(0);
      for(auto &cluster : cp.partitions()[ipart]) {
	if(ic > 0)
	  ofs_ << ";";
	ofs_ << cluster;
	++ic;
      }
      ofs_ << "," << cp.logprobs()[ipart]
	   << "," << cp.logweights()[ipart] << endl;
    }
    ++ipath;
  }
  ofs_.close();
}

// ----------------------------------------------------------------------------------------
double Glomerator::HammingFraction(Sequence seq_a, Sequence seq_b) {
  // NOTE since the cache is index by the joint key, this assumes we can arrive at this cluster via only one path. Which should be ok.
  string joint_key = JoinNames(seq_a.name(), seq_b.name());
  if(naive_hfracs_.count(joint_key))  // already did it (note that it's ok to cache naive seqs even when we're truncating, since each sequence, when part of a given group of sequence, always has the same length [it's different for forward because each key is compared in the likelihood ratio to many other keys, and each time its sequences can potentially have a different length]. In other words the difference is because we only calculate the naive sequence for sets of sequences that we've already merged.)
    return naive_hfracs_[joint_key];

  ++n_hfrac_calculated_;
  if(seq_a.size() != seq_b.size())
    throw runtime_error("ERROR sequences different length in Glomerator::HammingFraction (" + seq_a.undigitized() + "," + seq_b.undigitized() + ")\n");
  int distance(0), len_excluding_ambigs(0);
  for(size_t ic=0; ic<seq_a.size(); ++ic) {
    uint8_t ch_a(seq_a[ic]), ch_b(seq_b[ic]);
    if(ch_a == track_->ambiguous_index() || ch_b == track_->ambiguous_index())  // skip this position if either sequence has an ambiguous character (if not set, ambig-base should be the empty string)
      continue;
    ++len_excluding_ambigs;
    if(ch_a != ch_b)
      ++distance;
  }
  naive_hfracs_[joint_key] = distance / double(len_excluding_ambigs);
  return naive_hfracs_[joint_key];
}

// ----------------------------------------------------------------------------------------
double Glomerator::NaiveHammingFraction(string key_a, string key_b) {
  if(naive_hamming_fractions_.count(key_a + '-' + key_b))  // if we've already calculated this distance
    return naive_hamming_fractions_[key_a + '-' + key_b];

  GetNaiveSeq(key_a);
  GetNaiveSeq(key_b);
  double hfrac = HammingFraction(naive_seqs_[key_a], naive_seqs_[key_b]);  // hamming distance fcn will fail if the seqs aren't the same length
  naive_hamming_fractions_[key_a + '-' + key_b] = hfrac;  // add it with both key orderings... hackey, but only doubles the memory consumption
  naive_hamming_fractions_[key_b + '-' + key_a] = hfrac;
  return hfrac;
}

// ----------------------------------------------------------------------------------------
void Glomerator::GetNaiveSeq(string queries) {
  // <queries> is colon-separated list of query names
  if(naive_seqs_.count(queries)) {  // already did it (note that it's ok to cache naive seqs even when we're truncating, since each sequence, when part of a given group of sequence, always has the same length [it's different for forward because each key is compared in the likelihood ratio to many other keys, and each time its sequences can potentially have a different length]. In other words the difference is because we only calculate the naive sequence for sets of sequences that we've already merged.)
    ++n_vtb_cached_;
    return;
  }
  ++n_vtb_calculated_;

  Result result(kbinfo_[queries]);
  bool stop(false);
  do {
    // assert(SameLength(seq_info_[queries], true));
    result = vtb_dph_.Run(seq_info_[queries], kbinfo_[queries], only_genes_[queries], mute_freqs_[queries]);  // NOTE the sequences in <seq_info_[queries]> should already be the same length, since they've already been merged
    kbinfo_[queries] = result.better_kbounds();
    stop = !result.boundary_error() || result.could_not_expand();  // stop if the max is not on the boundary, or if the boundary's at zero or the sequence length
    if(args_->debug() && !stop)
      cout << "             expand and run again" << endl;  // note that subsequent runs are much faster than the first one because of chunk caching
  } while(!stop);

  if(result.events_.size() < 1)
    throw runtime_error("ERROR no events for " + queries + "\n");

  naive_seqs_[queries] = Sequence(track_, queries, result.events_[0].naive_seq_, result.events_[0].cyst_position_);
  if(result.boundary_error())
    errors_[queries] = errors_[queries] + ":boundary";
}

// ----------------------------------------------------------------------------------------
// add log prob for <name>/<seqs> to <log_probs_> (if it isn't already there)
void Glomerator::GetLogProb(string name, vector<Sequence> &seqs, KBounds &kbounds, vector<string> &only_genes, double mean_mute_freq) {
  // NOTE that when this improves the kbounds, that info doesn't get propagated to <kbinfo_>
  if(log_probs_.count(name)) {  // already did it (see note in GetNaiveSeq above)
    ++n_fwd_cached_;
    return;
  }
  ++n_fwd_calculated_;
    
  Result result(kbounds);
  bool stop(false);
  do {
    // assert(SameLength(seqs, true));
    result = fwd_dph_.Run(seqs, kbounds, only_genes, mean_mute_freq);  // NOTE <only_genes> isn't necessarily <only_genes_[name]>, since for the denominator calculation we take the OR
    kbounds = result.better_kbounds();
    stop = !result.boundary_error() || result.could_not_expand();  // stop if the max is not on the boundary, or if the boundary's at zero or the sequence length
    if(args_->debug() && !stop)
      cout << "             expand and run again" << endl;  // note that subsequent runs are much faster than the first one because of chunk caching
  } while(!stop);

  log_probs_[name] = result.total_score();
  if(result.boundary_error() && !result.could_not_expand())  // could_not_expand means the max is at the edge of the sequence -- e.g. k_d min is 1
    errors_[name] = errors_[name] + ":boundary";
}

// ----------------------------------------------------------------------------------------
vector<Sequence> Glomerator::MergeSeqVectors(string name_a, string name_b) {
  // NOTE does *not* truncate anything

  // first merge the two vectors
  vector<Sequence> merged_seqs;  // NOTE doesn't work if you preallocate and use std::vector::insert(). No, I have no *@#*($!#ing idea why
  for(size_t is=0; is<seq_info_[name_a].size(); ++is)
    merged_seqs.push_back(seq_info_[name_a][is]);
  for(size_t is=0; is<seq_info_[name_b].size(); ++is)
    merged_seqs.push_back(seq_info_[name_b][is]);

  // then make sure we don't have the same sequence twice
  set<string> all_names;
  for(size_t is=0; is<merged_seqs.size(); ++is) {
    string name(merged_seqs[is].name());
    if(all_names.count(name))
      throw runtime_error("tried to add sequence with name " + name + " twice in Glomerator::MergeSeqVectors()");
    else
      all_names.insert(name);
  }

  return merged_seqs;
}

// ----------------------------------------------------------------------------------------
bool Glomerator::SameLength(vector<Sequence> &seqs, bool debug) {
  // are all the seqs in <seqs> the same length?
  bool same(true);
  int len(-1);
  for(auto &seq : seqs) {
    if(len < 0)
      len = (int)seq.size();
    if(len != (int)seq.size()) {
      same = false;
      break;
    }
  }
  if(debug) {
    if(same)
      cout << "same length: " << JoinNameStrings(seqs) << endl;
    else
      cout << "not same length! " << JoinNameStrings(seqs) << "\n" << JoinSeqStrings(seqs, "\n") << endl;
  }

  return same;
}  

// ----------------------------------------------------------------------------------------
Query Glomerator::GetMergedQuery(string name_a, string name_b) {
  // NOTE truncates sequences!

  Query qmerged;
  qmerged.name_ = JoinNames(name_a, name_b);  // sorts name_a and name_b, but *doesn't* sort within them
  qmerged.seqs_ = MergeSeqVectors(name_a, name_b);  // doesn't truncate anything

  // truncation
  assert(SameLength(seq_info_[name_a]));  // all the seqs for name_a should already be the same length
  assert(SameLength(seq_info_[name_b]));  // ...same for name_b
  vector<KBounds> kbvector;  // make vector of kbounds, with first chunk corresponding to <name_a>, and the second to <name_b>. This shenaniganery is necessary so we can take the OR of the two kbounds *after* truncation
  for(size_t is=0; is<qmerged.seqs_.size(); ++is) {
    if(is < seq_info_[name_a].size())  // the first part of the vector is for sequences from name_a
      kbvector.push_back(kbinfo_[name_a]);
    else
      kbvector.push_back(kbinfo_[name_b]);  // ... and second part is for those from name_b
  }

  qmerged.kbounds_ = kbvector[0].LogicalOr(kbvector.back());  // OR of the kbounds for name_a and name_b, after they've been properly truncated

  qmerged.only_genes_ = only_genes_[name_a];
  for(auto &g : only_genes_[name_b])  // NOTE this will add duplicates (that's no big deal, though) OPTIMIZATION
    qmerged.only_genes_.push_back(g);
  // TODO mute freqs aren't really right any more if we truncated things above
  qmerged.mean_mute_freq_ = (seq_info_[name_a].size()*mute_freqs_[name_a] + seq_info_[name_b].size()*mute_freqs_[name_b]) / double(qmerged.seqs_.size());  // simple weighted average (doesn't account for different sequence lengths)
  qmerged.parents_ = pair<string, string>(name_a, name_b);
  return qmerged;
}

// ----------------------------------------------------------------------------------------
Query *Glomerator::ChooseRandomMerge(vector<pair<double, Query> > &potential_merges, smc::rng *rgen) {
  // first leave log space and normalize. NOTE instead of normalizing, we could just use rng::Uniform(0., total)
  vector<double> ratios;  // NOTE *not* a probability: it's the ratio of the probability together to the probability apart
  double total(0.0);
  for(auto &pr : potential_merges) {
    double likelihood_ratio = exp(pr.first);
    total += likelihood_ratio;
    ratios.push_back(likelihood_ratio);
  }
  for(auto &ratio : ratios)
    ratio /= total;

  // then choose one at random according to the probs
  double drawpoint = rgen->Uniform(0., 1.);
  double sum(0.0);
  for(size_t im=0; im<ratios.size(); ++im) {
    sum += ratios[im];
    if(sum > drawpoint) {
      return &potential_merges[im].second;
    }
  }

  throw runtime_error("fell through in Glomerator::ChooseRandomMerge");
}

// ----------------------------------------------------------------------------------------
string Glomerator::JoinNames(string name1, string name2) {
  vector<string> names{name1, name2};
  sort(names.begin(), names.end());  // NOTE this doesn't sort *within* name1 or name2 when they're already comprised of several uids. In principle this will lead to unnecessary cache misses (if we later arrive at the same combination of sequences from a different starting point). In practice, this is very unlikely (unless we're dong smc) since we've already merged the constituents of name1 and name2 and we can't unmerge them.
  return names[0] + ":" + names[1];
}

// ----------------------------------------------------------------------------------------
string Glomerator::JoinNameStrings(vector<Sequence> &strlist, string delimiter) {
  string return_str;
  for(size_t is=0; is<strlist.size(); ++is) {
    if(is > 0)
      return_str += delimiter;
    return_str += strlist[is].name();
  }
  return return_str;
}

// ----------------------------------------------------------------------------------------
string Glomerator::JoinSeqStrings(vector<Sequence> &strlist, string delimiter) {
  string return_str;
  for(size_t is=0; is<strlist.size(); ++is) {
    if(is > 0)
      return_str += delimiter;
    return_str += strlist[is].undigitized();
  }
  return return_str;
}

// ----------------------------------------------------------------------------------------
Query Glomerator::ChooseMerge(ClusterPath *path, smc::rng *rgen, double *chosen_lratio) {
  double max_log_prob(-INFINITY), min_hamming_fraction(INFINITY);
  Query min_hamming_merge;
  int imax(-1);
  vector<pair<double, Query> > potential_merges;
  int n_total_pairs(0), n_skipped_hamming(0), n_inf_factors(0);
  for(Partition::iterator it_a = path->CurrentPartition().begin(); it_a != path->CurrentPartition().end(); ++it_a) {
    for(Partition::iterator it_b = it_a; ++it_b != path->CurrentPartition().end();) {
      string key_a(*it_a), key_b(*it_b);
      ++n_total_pairs;

      if(NaiveHammingFraction(key_a, key_b) > args_->hamming_fraction_bound_hi()) {  // if naive hamming fraction too big, don't even consider merging the pair
	++n_skipped_hamming;
	continue;
      }

      Query qmerged(GetMergedQuery(key_a, key_b));

      if(args_->hamming_fraction_bound_lo() > 0.0 && NaiveHammingFraction(key_a, key_b) < args_->hamming_fraction_bound_lo()) {  // if naive hamming is small enough, merge the pair without running hmm
	if(NaiveHammingFraction(key_a, key_b) < min_hamming_fraction) {
	  min_hamming_fraction = NaiveHammingFraction(key_a, key_b);
	  min_hamming_merge = qmerged;
	}
	continue;
      }
      if(min_hamming_fraction < INFINITY)  // if we have any potential hamming merges, that means we'll do those before we do any hmm tomfoolery
	continue;

      // NOTE the error from using the single kbounds rather than the OR seems to be around a part in a thousand or less
      // NOTE also that the _a and _b results will be cached (unless we're truncating), but with their *individual* only_gene sets (rather than the OR)... but this seems to be ok.

      // TODO if kbounds gets expanded in one of these three calls, we don't redo the others. Which is really ok, but could be checked again?
      GetLogProb(key_a, seq_info_[key_a], qmerged.kbounds_, qmerged.only_genes_, mute_freqs_[key_a]);
      GetLogProb(key_b, seq_info_[key_b], qmerged.kbounds_, qmerged.only_genes_, mute_freqs_[key_b]);
      GetLogProb(qmerged.name_, qmerged.seqs_, qmerged.kbounds_, qmerged.only_genes_, qmerged.mean_mute_freq_);

      double lratio(log_probs_[qmerged.name_] - log_probs_[key_a] - log_probs_[key_b]);  // REMINDER a, b not necessarily same order as names[0], names[1]
      if(args_->debug()) {
	printf("       %8.3f = ", lratio);
	printf("%2s %8.2f", "", log_probs_[qmerged.name_]);
	printf(" - %8.2f - %8.2f", log_probs_[key_a], log_probs_[key_b]);
	printf("\n");
      }

      // ----------------------------------------------------------------------------------------
      // this should really depend on the sample's mutation frequency as well
      if(qmerged.seqs_.size() == 2 && lratio < 20.) {
      	continue;
      } else if(qmerged.seqs_.size() == 3 && lratio < 15.) {
      	continue;
      } else if(qmerged.seqs_.size() == 4 && lratio < 10.) {
      	continue;
      } else if(qmerged.seqs_.size() == 5 && lratio < 5.) {
      	continue;
      }
      // ----------------------------------------------------------------------------------------

      potential_merges.push_back(pair<double, Query>(lratio, qmerged));

      if(lratio == -INFINITY)
	++n_inf_factors;

      if(lratio > max_log_prob) {
	max_log_prob = lratio;
	imax = potential_merges.size() - 1;
      }
    }
  }

  if(min_hamming_fraction < INFINITY) {  // if lower hamming fraction was set, then merge the best (nearest) pair of clusters
    ++n_hamming_merged_;
    *chosen_lratio = -INFINITY;  // er... or something
    return min_hamming_merge;
  }

  if(args_->debug())
    printf("          hamming skipped %d / %d\n", n_skipped_hamming, n_total_pairs);

  // if <path->CurrentPartition()> only has one cluster, if hamming is too large between all remaining clusters, or if remaining likelihood ratios are -INFINITY
  if(max_log_prob == -INFINITY) {
    if(path->CurrentPartition().size() == 1)
      cout << "        stop with partition of size one" << endl;
    else if(n_skipped_hamming == n_total_pairs)
      cout << "        stop with all " << n_skipped_hamming << " / " << n_total_pairs << " hamming distances greater than " << args_->hamming_fraction_bound_hi() << endl;
    else if(n_inf_factors == n_total_pairs)
      cout << "        stop with all " << n_inf_factors << " / " << n_total_pairs << " likelihood ratios -inf" << endl;
    else
      cout << "        stop for some reason or other with -inf: " << n_inf_factors << "   ham skip: " << n_skipped_hamming << "   total: " << n_total_pairs << endl;

    path->finished_ = true;
    return Query();
  }

  if(args_->smc_particles() == 1) {
    *chosen_lratio = potential_merges[imax].first;
    return potential_merges[imax].second;
  } else {
    Query *chosen_qmerge = ChooseRandomMerge(potential_merges, rgen);
    *chosen_lratio = log_probs_[chosen_qmerge->name_];
    return *chosen_qmerge;
  }
}

// ----------------------------------------------------------------------------------------
// perform one merge step, i.e. find the two "nearest" clusters and merge 'em (unless we're doing doing smc, in which case we choose a random merge accordingy to their respective nearnesses)
void Glomerator::Merge(ClusterPath *path, smc::rng *rgen) {
  if(path->finished_)  // already finished this <path>, but we're still iterating 'cause some of the other paths aren't finished
    return;

  double chosen_lratio;
  Query chosen_qmerge = ChooseMerge(path, rgen, &chosen_lratio);
  if(path->finished_)
    return;

  // add query info for the chosen merge, unless we already did it (e.g. if another particle already did this merge)
  if(seq_info_.count(chosen_qmerge.name_) == 0) {  // if we're doing smc, this will happen once for each particle that wants to merge these two. NOTE you get a very, very strange seg fault at the Sequences::Union line above, I *think* inside the implicit copy constructor. Yes, I should just define my own copy constructor, but I can't work out how to navigate through the const jungle a.t.m.
    seq_info_[chosen_qmerge.name_] = chosen_qmerge.seqs_;
    kbinfo_[chosen_qmerge.name_] = chosen_qmerge.kbounds_;
    mute_freqs_[chosen_qmerge.name_] = chosen_qmerge.mean_mute_freq_;
    only_genes_[chosen_qmerge.name_] = chosen_qmerge.only_genes_;
    GetNaiveSeq(chosen_qmerge.name_);
  }

  double last_partition_logprob(LogProbOfPartition(path->CurrentPartition()));
  Partition new_partition(path->CurrentPartition());  // note: CurrentPartition() returns a reference
  new_partition.erase(chosen_qmerge.parents_.first);
  new_partition.erase(chosen_qmerge.parents_.second);
  new_partition.insert(chosen_qmerge.name_);
  path->AddPartition(new_partition, LogProbOfPartition(new_partition), args_->max_logprob_drop());

  if(args_->debug()) {
    // cout << "    path " << path->initial_path_index_ << endl;
    // printf("       merged %-8.2f %s and %s\n", max_log_prob, max_pair.first.c_str(), max_pair.second.c_str());
    // assert(SameLength(chosen_qmerge.seqs_, true));
    printf("       merged %-8.2f", chosen_lratio);
    double newdelta = LogProbOfPartition(new_partition) - last_partition_logprob;
    if(fabs(newdelta - chosen_lratio) > 1e-8)
      printf(" ( %-20.15f != %-20.15f)", chosen_lratio, LogProbOfPartition(new_partition) - last_partition_logprob);
    printf("   %s and %s\n", chosen_qmerge.parents_.first.c_str(), chosen_qmerge.parents_.second.c_str());
    string extrastr("current (logweight " + to_string(path->CurrentLogWeight()) + ")");
    PrintPartition(new_partition, extrastr);
  }
}

// ----------------------------------------------------------------------------------------
ClusterPair Glomerator::GetClustersToMerge(set<vector<string> > &clusters, int max_per_cluster, bool merge_whatever_you_got) {
  double smallest_min_distance(9999);
  ClusterPair clusters_to_merge;
  int n_skipped(0);
  for(set<vector<string> >::iterator clust_a = clusters.begin(); clust_a != clusters.end(); ++clust_a) {
    for(set<vector<string> >::iterator clust_b = clust_a; ++clust_b != clusters.end();) {
      if(!merge_whatever_you_got && clust_a->size() + clust_b->size() > (size_t)max_per_cluster) {  // merged cluster would be too big, so look for smaller (albeit further-apart) things to merge
	++n_skipped;
	continue;
      }

      double min_distance(9999);  // find the smallest hamming distance between any two sequences in the two clusters
      for(auto &query_a : *clust_a) {
	for(auto &query_b : *clust_b) {
	  string key(query_a + "-" + query_b);
	  double hfrac;
	  if(hamming_fractions_.count(key) == 0) {
	    hfrac = NaiveHammingFraction(query_a, query_b);
	    hamming_fractions_[key] = hfrac;
	    hamming_fractions_[query_b + "-" + query_a] = hfrac;  // also add the reverse-ordered key
	  } else {
	    hfrac = hamming_fractions_[key];
	  }
	  if(hfrac < min_distance)
	    min_distance = hfrac;
	}
      }

      if(min_distance < smallest_min_distance) {
	smallest_min_distance = min_distance;
	// vector<string> &ref_a(*clust_a), &ref_b(*clust_b);
	clusters_to_merge = ClusterPair(*clust_a, *clust_b);
      }
      // if(args_->debug() && n_skipped > 0)
      // 	printf("      skipped: %d", n_skipped);
    }
  }
  return clusters_to_merge;
}

// ----------------------------------------------------------------------------------------
void Glomerator::PrintClusterSizes(set<vector<string> > &clusters) {
  for(auto &cl : clusters)
    cout << " " << cl.size();
  cout << endl;
}

// ----------------------------------------------------------------------------------------
ClusterPair Glomerator::GetSmallBigClusters(set<vector<string> > &clusters) { // return the samllest and biggest clusters
  ClusterPair smallbig;
  for(auto &clust : clusters) {
    if(smallbig.first.size() == 0 || clust.size() < smallbig.first.size())
      smallbig.first = clust;
    if(smallbig.second.size() == 0 || clust.size() > smallbig.second.size())
      smallbig.second = clust;
  }
  return smallbig;
}

// ----------------------------------------------------------------------------------------
// perform one merge step, i.e. find the two "nearest" clusters and merge 'em (unless we're doing doing smc, in which case we choose a random merge accordingy to their respective nearnesses)
void Glomerator::NaiveSeqGlomerate(int n_clusters) {
  clock_t run_start(clock());
  // clusters = [[names,] for names in naive_seqs.keys()]
  double seqs_per_cluster = double(seq_info_.size()) / n_clusters;
  int max_per_cluster = ceil(seqs_per_cluster);
  if(args_->debug())
    printf("  making %d clusters (max %d per cluster)\n", n_clusters, max_per_cluster);

  set<vector<string> > clusters;
  for(auto &kv : seq_info_)
    clusters.insert(vector<string>{kv.first});

  if(args_->debug())
    PrintClusterSizes(clusters);

  bool merge_whatever_you_got(false);
  while(clusters.size() > (size_t)n_clusters) {
    // if(args_->debug())//   printf'    current ', ' '.join([str(len(cl)) for cl in clusters])
    ClusterPair clusters_to_merge = GetClustersToMerge(clusters, max_per_cluster, merge_whatever_you_got);
    if(clusters_to_merge.first.size() == 0) {  // if we didn't find a suitable pair
      // if debug://     print '    didn\'t find shiznitz'
      merge_whatever_you_got = true;  // next time through, merge whatever's best regardless of size
    } else {
      // if debug:print '    merging', len(clusters_to_merge[0]), len(clusters_to_merge[1])
      vector<string> new_cluster(clusters_to_merge.first);
      new_cluster.insert(new_cluster.end(), clusters_to_merge.second.begin(), clusters_to_merge.second.end());  // add clusters from the second cluster to the first cluster
      clusters.insert(new_cluster);
      clusters.erase(clusters_to_merge.first);  // then erase the old clusters
      clusters.erase(clusters_to_merge.second);
      if(args_->debug())
	PrintClusterSizes(clusters);
    }
  }

  size_t itries(0);
  ClusterPair smallbig = GetSmallBigClusters(clusters);
  while(float(smallbig.second.size()) / smallbig.first.size() > 1.1 && smallbig.second.size() - smallbig.first.size() > 3) {  // keep homogenizing while biggest cluster is more than 3/2 the size of the smallest (and while their sizes differ by more than 2)
    if(args_->debug())
      cout << "homogenizing" << endl;
    int n_to_keep_in_biggest_cluster = ceil(double(smallbig.first.size() + smallbig.second.size()) / 2);
    clusters.erase(smallbig.first);
    clusters.erase(smallbig.second);
    smallbig.first.insert(smallbig.first.end(), smallbig.second.begin() + n_to_keep_in_biggest_cluster, smallbig.second.end());
    smallbig.second.erase(smallbig.second.begin() + n_to_keep_in_biggest_cluster, smallbig.second.end());
    clusters.insert(smallbig.first);
    clusters.insert(smallbig.second);
    if(args_->debug())
      PrintClusterSizes(clusters);
    ++itries;
    if(itries > clusters.size()) {
      // if debug: print '  too many homogenization tries'
      break;
    }
    smallbig = GetSmallBigClusters(clusters);
  }

  // cout << "  final bcrham divvy" << endl;
  // int tmpic(0);
  // for(auto &clust : clusters) {
  //   cout << "      " << tmpic << endl;
  //   for(auto &query : clust)
  //     cout << "          " << query << endl;
  //   ++tmpic;
  // }

  ofs_.open(args_->outfile());
  ofs_ << "partition" << endl;
  int ic(0);
  for(auto &clust : clusters) {
    if(ic > 0)
      ofs_ << "|";
    int iq(0);
    for(auto &query : clust) {
      if(iq > 0)
	ofs_ << ";";
      ofs_ << query;
      ++iq;
    }
    ++ic;
  }
  ofs_ << endl;
  ofs_.close();

  cout << "        naive hamming cluster time " << ((clock() - run_start) / (double)CLOCKS_PER_SEC) << "   to condense " << seq_info_.size() << " --> " << n_clusters <<  endl;
}

}

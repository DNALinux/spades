//***************************************************************************
//* Copyright (c) 2011-2013 Saint-Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//****************************************************************************

/*
 * main.cpp
 *
 *  Created on: 08.07.2011
 *      Author: snikolenko
 */
 
#include "standard.hpp"
#include "logger/logger.hpp"

#include <cmath>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cassert>

#include "read/ireadstream.hpp"
#include "adt/concurrent_dsu.hpp"
#include "segfault_handler.hpp"
#include "config_struct_hammer.hpp"
#include "hammer_tools.hpp"
#include "kmer_cluster.hpp"
#include "position_read.hpp"
#include "globals.hpp"
#include "kmer_data.hpp"

#include "memory_limit.hpp"
#include "logger/log_writers.hpp"

std::vector<std::string> Globals::input_filenames = std::vector<std::string>();
std::vector<std::string> Globals::input_filename_bases = std::vector<std::string>();
std::vector<hint_t> Globals::input_file_blob_positions = std::vector<hint_t>();
std::vector<size_t> Globals::input_file_sizes = std::vector<size_t>();
std::vector<uint32_t> * Globals::subKMerPositions = NULL;
KMerData *Globals::kmer_data = NULL;
int Globals::iteration_no = 0;

hint_t Globals::blob_size = 0;
hint_t Globals::blob_max_size = 0;
char * Globals::blob = NULL;
char * Globals::blobquality = NULL;
size_t Globals::read_length = 0;

char Globals::char_offset = 0;
bool Globals::char_offset_user = true;

bool Globals::use_common_quality = false;
char Globals::common_quality = 0;
double Globals::common_kmer_errprob = 0;
double Globals::quality_probs[256] = { 0 };
double Globals::quality_lprobs[256] = { 0 };
double Globals::quality_rprobs[256] = { 0 };
double Globals::quality_lrprobs[256] = { 0 };

std::vector<PositionRead> * Globals::pr = NULL;

struct UfCmp {
  bool operator()(const std::vector<int> &lhs, const std::vector<int> &rhs) {
    return (lhs[0] < rhs[0]);
  }
};

void create_console_logger() {
	using namespace logging;

	logger *lg = create_logger("");
	lg->add_writer(std::make_shared<console_writer>());
  attach_logger(lg);
}
 
int main(int argc, char * argv[]) {
  segfault_handler sh;

  srand(42);
  srandom(42);

  try {
    create_console_logger();

    string config_file = CONFIG_FILENAME;
    if (argc > 1) config_file = argv[1];
    INFO("Loading config from " << config_file.c_str());
    cfg::create_instance(config_file);

    // general config parameters
    Globals::use_common_quality = cfg::get().common_quality > 0;
    Globals::common_quality = (char)cfg::get().common_quality;
    Globals::common_kmer_errprob = 1.0;
    for (size_t i=0; i < hammer::K; ++i)
      Globals::common_kmer_errprob *= 1 - pow(10.0, - Globals::common_quality / 10.0);
    Globals::common_kmer_errprob = 1 - Globals::common_kmer_errprob;

    // hard memory limit
    const size_t GB = 1 << 30;
    limit_memory(cfg::get().general_hard_memory_limit * GB);

    // input files with reads
    if (cfg::get().input_paired_1 != "" && cfg::get().input_paired_2 != "") {
      Globals::input_filenames.push_back(cfg::get().input_paired_1);
      Globals::input_filenames.push_back(cfg::get().input_paired_2);
    }
    if (cfg::get().input_single != "") Globals::input_filenames.push_back(cfg::get().input_single);

    VERIFY(Globals::input_filenames.size() > 0);

    for (size_t iFile=0; iFile < Globals::input_filenames.size(); ++iFile) {
      Globals::input_filename_bases.push_back(
          path::basename(Globals::input_filenames[iFile]) +
          path::extension(Globals::input_filenames[iFile]));
      INFO("Input file: " << Globals::input_filename_bases[iFile]);
    }

    // decompress input reads if they are gzipped
    HammerTools::DecompressIfNeeded();

    // determine quality offset if not specified
    if (!cfg::get().input_qvoffset_opt) {
      INFO("Trying to determine PHRED offset");
      int determined_offset = determine_offset(Globals::input_filenames.front());
      if (determined_offset < 0) {
    	ERROR("Failed to determine offset! Specify it manually and restart, please!");
        return 0;
      } else {
        INFO("Determined value is " << determined_offset);
        cfg::get_writable().input_qvoffset = determined_offset;
      }
      Globals::char_offset_user = false;
    } else {
      cfg::get_writable().input_qvoffset = *cfg::get().input_qvoffset_opt;
      Globals::char_offset_user = true;
    }
    Globals::char_offset = (char)cfg::get().input_qvoffset;

    // Pre-cache quality probabilities
    for (unsigned qual = 0; qual < sizeof(Globals::quality_probs) / sizeof(Globals::quality_probs[0]); ++qual) {
      Globals::quality_rprobs[qual] = (qual < 3 ? 0.75 : pow(10.0, -(int)qual / 10.0));
      Globals::quality_probs[qual] = 1 - Globals::quality_rprobs[qual];
      Globals::quality_lprobs[qual] = log(Globals::quality_probs[qual]);
      Globals::quality_lrprobs[qual] = log(Globals::quality_rprobs[qual]);
    }

    // if we need to change single Ns to As, this is the time
    if (cfg::get().general_change_n_to_a && cfg::get().count_do) {
      INFO("Changing single Ns to As in input read files.");
      HammerTools::ChangeNtoAinReadFiles();
      INFO("Single Ns changed, " << Globals::input_filenames.size() << " read files written.");
    }

    // estimate total read size
    size_t totalReadSize = hammer_tools::EstimateTotalReadSize(Globals::input_filenames);
    INFO("Estimated total size of all reads is " << totalReadSize);

    // allocate the blob
    Globals::blob_size = totalReadSize + 1;
    Globals::blob_max_size = Globals::blob_size;
    Globals::blob = new char[Globals::blob_max_size];
    if (!Globals::use_common_quality) Globals::blobquality = new char[Globals::blob_max_size];
    INFO("Max blob size as allocated is " << Globals::blob_max_size);

    // initialize subkmer positions
    HammerTools::InitializeSubKMerPositions();

    int max_iterations = cfg::get().general_max_iterations;

    // now we can begin the iterations
    for (Globals::iteration_no = 0; Globals::iteration_no < max_iterations; ++Globals::iteration_no) {
      cout << "\n     === ITERATION " << Globals::iteration_no << " begins ===" << endl;
      bool do_everything = cfg::get().general_do_everything_after_first_iteration && (Globals::iteration_no > 0);
      
      // initialize k-mer structures
      Globals::kmer_data = new KMerData;

      // read input reads into blob
      Globals::pr = new vector<PositionRead>();
      HammerTools::ReadAllFilesIntoBlob();

      // count k-mers
      if (cfg::get().count_do || do_everything) {
        KMerDataCounter counter(cfg::get().count_numfiles);
        counter.FillKMerData(*Globals::kmer_data);

        if (cfg::get().general_debug) {
          INFO("Debug mode on. Dumping K-mer index");
          std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index");
          std::ofstream os(fname.c_str(), std::ios::binary);
          Globals::kmer_data->binary_write(os);
        }
      } else {
        INFO("Reading K-mer index");
        std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index");
        std::ifstream is(fname.c_str(), std::ios::binary);
        VERIFY(is.good());
        Globals::kmer_data->binary_read(is);
      }

      // Cluster the Hamming graph
      std::vector<std::vector<unsigned> > classes;
      if (cfg::get().hamming_do || do_everything) {
        ConcurrentDSU uf(Globals::kmer_data->size());
        KMerHamClusterer clusterer(cfg::get().general_tau);
        INFO("Clustering Hamming graph.");
        clusterer.cluster(HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.hamcls"),
                          *Globals::kmer_data, uf);
        uf.get_sets(classes);
        size_t num_classes = classes.size();

#if 0
        std::sort(classes.begin(), classes.end(),  UfCmp());
        for (size_t i = 0; i < classes.size(); ++i) {
          std::cerr << i << ": { ";
          for (size_t j = 0; j < classes[i].size(); ++j)
            std::cerr << classes[i][j] << ", ";
          std::cerr << "}" << std::endl;
        }
#endif
        INFO("Clustering done. Total clusters: " << num_classes);

        if (cfg::get().general_debug) {
          INFO("Debug mode on. Writing down clusters.");

          std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.hamming");
          std::ofstream ofs(fname.c_str(),
                            std::ios::binary | std::ios::out);

          ofs.write((char*)&num_classes, sizeof(num_classes));
          for (size_t i=0; i < classes.size(); ++i) {
            size_t sz = classes[i].size();
            ofs.write((char*)&sz, sizeof(sz));
            ofs.write((char*)&classes[i][0], sz * sizeof(classes[i][0]));
          }
        }
      } else {
        INFO("Reading clusters");

        std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.hamming");
        std::ifstream is(fname.c_str(), std::ios::binary | std::ios::in);
        VERIFY(is.good());

        size_t num_classes = 0;
        is.read((char*)&num_classes, sizeof(num_classes));
        classes.resize(num_classes);

        for (size_t i = 0; i < num_classes; ++i) {
          size_t sz = 0;
          is.read((char*)&sz, sizeof(sz));
          classes[i].resize(sz);
          is.read((char*)&classes[i][0], sz * sizeof(classes[i][0]));
        }
      }

      if (cfg::get().bayes_do || do_everything) {
        INFO("Subclustering Hamming graph");
        unsigned clustering_nthreads = std::min(cfg::get().general_max_nthreads, cfg::get().bayes_nthreads);
        KMerClustering kmc(*Globals::kmer_data, clustering_nthreads, cfg::get().input_working_dir);
        kmc.process(classes);
        INFO("Finished clustering.");

        if (cfg::get().general_debug) {
          INFO("Debug mode on. Dumping K-mer index");
          std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index2");
          std::ofstream os(fname.c_str(), std::ios::binary);
          Globals::kmer_data->binary_write(os);
        }
      } else {
        INFO("Reading K-mer index");
        std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index2");
        std::ifstream is(fname.c_str(), std::ios::binary);
        VERIFY(is.good());
        Globals::kmer_data->binary_read(is);
      }
      std::vector<std::vector<unsigned>>().swap(classes);

      // expand the set of solid k-mers
      if (cfg::get().expand_do || do_everything) {
        unsigned expand_nthreads = std::min(cfg::get().general_max_nthreads, cfg::get().expand_nthreads);
        INFO("Starting solid k-mers expansion in " << expand_nthreads << " threads.");
        for (unsigned expand_iter_no = 0; expand_iter_no < cfg::get().expand_max_iterations; ++expand_iter_no) {
          size_t res = HammerTools::IterativeExpansionStep(expand_iter_no, expand_nthreads, *Globals::kmer_data);
          INFO("Solid k-mers iteration " << expand_iter_no << " produced " << res << " new k-mers.");
          if (res < 10) break;
        }
        INFO("Solid k-mers finalized");

        if (cfg::get().general_debug) {
          INFO("Debug mode on. Dumping K-mer index");
          std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index3");
          std::ofstream os(fname.c_str(), std::ios::binary);
          Globals::kmer_data->binary_write(os);
        }
      } else {
        INFO("Reading K-mer index");
        std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmer.index3");
        std::ifstream is(fname.c_str(), std::ios::binary);
        VERIFY(is.good());
        Globals::kmer_data->binary_read(is);
      }

      size_t totalReads = 0;
      // reconstruct and output the reads
      if (cfg::get().correct_do || do_everything) {
        totalReads = HammerTools::CorrectAllReads();
      }

      // prepare the reads for next iteration
      // delete consensuses, clear kmer data, and restore correct revcomps
      delete Globals::kmer_data;
      delete Globals::pr;

      if (totalReads < 1) {
        INFO("Too few reads have changed in this iteration. Exiting.");
        break;
      }
      // break;
    }

    // clean up
    Globals::subKMerPositions->clear();
    delete Globals::subKMerPositions;
    delete [] Globals::blob;
    delete [] Globals::blobquality;

    INFO("All done. Exiting.");
  } catch (std::bad_alloc const& e) {
    std::cerr << "Not enough memory to run BayesHammer. " << e.what() << std::endl;
    return EINTR;
  }

  return 0;
}



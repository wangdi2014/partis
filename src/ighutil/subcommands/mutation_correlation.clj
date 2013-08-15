(ns ighutil.subcommands.mutation-correlation
  (:import [net.sf.samtools
            SAMRecord
            SAMFileHeader
            SAMFileReader
            SAMFileReader$ValidationStringency
            SAMSequenceRecord]
           [net.sf.picard.reference
            ReferenceSequenceFileFactory])
  (:require [cheshire.core :refer [generate-stream]]
            [cheshire.generate :refer [add-encoder remove-encoder encode-seq]]
            [clojure.core.reducers :as r]
            [clojure.data.csv :as csv]
            [clojure.java.io :as io]
            [cliopatra.command :refer [defcommand]]
            [hiphip.long :as long]
            [ighutil.fasta :refer [extract-references]]
            [ighutil.io :as zio]
            [ighutil.sam :as sam]
            [ighutil.sam-tags :refer [TAG-EXP-MATCH]]
            [plumbing.core :refer [safe-get map-vals]]
            [primitive-math :as p]))


(defn- result-skeleton [refs]
  (into
   {}
   (for [[name ^bytes bases] refs]
     (let [length (alength bases)
           msize (* length length)]
       [name {:length length
              :bases (String. bases)
              :mutated (long-array msize)
              :unmutated (long-array msize)
              :count (long-array length)}] ))))

(defn- finalize-result [result]
  (map-vals
   (fn [{:keys [mutated unmutated count length] :as m}]
     (assoc m
       :mutated (vec mutated)
       :unmutated (vec unmutated)
       :count (vec count)))
   result))

(defn- unmask-base-exp-match [^SAMRecord read ^bytes bq ref-len]
  "Generates two arrays: [counts mutations]
  counts contains 1/0 indicating whether a base aligns to each
  position.

  mutations contains 1/0 indicating whether there is a mutation at
  each position."
  (let [counts (long-array ref-len 0)
        matches (long/aclone counts)]
    (doseq [i (range (alength bq))]
      (let [ref-idx (.getReferencePositionAtReadPosition read i)
            b (long (aget bq i))]
        (when (and (not= 0 ref-idx) (>= b 0) (= 0 (mod b 100)))
          (let [ref-idx (dec (int ref-idx))]
            (long/aset counts ref-idx 1)
            (long/aset matches ref-idx (- 1 (/ b 100)))))))
    [counts matches]))

(defn- match-by-site-of-read [^SAMRecord read
                              {:keys [length mutated unmutated count]}]
  "Given a SAM record with the TAG-EXP-MATCH tag,
   generates a vector of
   [reference-name {site-index {matches-at-site }}]"
  (let [length (int length)
        ^bytes bq (.getAttribute read TAG-EXP-MATCH)
        [^longs read-counts
         ^longs read-muts] (unmask-base-exp-match read bq length)]
    (assert (not (nil? bq)))
    (long/afill! [c count i read-counts] (p/+ i c))
    (doseq [i (range (alength read-muts))]
      (let [i (int i)
            ^longs tgt (if (p/== 0 (long/aget read-muts i))
                         unmutated
                         mutated)]
        (long/doarr [[j c] read-muts]
                    (long/ainc tgt (p/+ (p/* i length) j) c))))))

(defn- match-by-site-of-records [refs records]
  "Get the number of records that match / mismatch at each other site
   conditioning on a mutation in each position"
  (let [result (result-skeleton refs)]
    (doseq [^SAMRecord read records]
      (match-by-site-of-read
       read
       (safe-get result  (sam/reference-name read))))
    result))

(defcommand mutation-correlation
  "Set up for summarizing mutation correlation"
  {:opts-spec [["-i" "--in-file" "Source file" :required true
                :parse-fn io/file]
               ["-r" "--reference-file" "Reference file" :required true
                :parse-fn io/file]
               ["-o" "--out-file" "Destination path" :required true
                :parse-fn zio/writer]]}
  (assert (not= in-file out-file))
  (assert (.exists ^java.io.File in-file))
  (with-open [reader (SAMFileReader. ^java.io.File in-file)
              writer ^java.io.Closeable out-file]
    (.setValidationStringency
     reader
     SAMFileReader$ValidationStringency/SILENT)
    (let [long-array-cls (resolve (symbol "[J"))
          ref-map (->> reference-file
                       ReferenceSequenceFileFactory/getReferenceSequenceFile
                       extract-references
                       (into {}))
          m (->> reader
                 .iterator
                 iterator-seq
                 (match-by-site-of-records ref-map))]
      (add-encoder long-array-cls encode-seq)
      (println "Finished: writing results")
      (try
        (generate-stream m writer)
        (finally (remove-encoder long-array-cls)))))
  nil)

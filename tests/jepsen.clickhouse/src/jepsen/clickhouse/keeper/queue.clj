(ns jepsen.clickhouse.keeper.queue
  (:require
   [clojure.tools.logging :refer :all]
   [jepsen
    [checker :as checker]
    [client :as client]
    [generator :as gen]]
   [knossos.model :as model]
   [jepsen.checker.timeline :as timeline]
   [jepsen.clickhouse.keeper.utils :refer :all]
   [jepsen.clickhouse.utils :as chu]
   [zookeeper :as zk])
  (:import (org.apache.zookeeper ZooKeeper KeeperException KeeperException$BadVersionException)))

(def root-path "/queue")
(defn enqueue   [val _ _] {:type :invoke, :f :enqueue :value val})
(defn dequeue [_ _] {:type :invoke, :f :dequeue})

(defrecord QueueClient [conn nodename]
  client/Client
  (open! [this test node]
    (assoc
     (assoc this
            :conn (zk-connect node 9181 30000 (:with-auth test)))
     :nodename node))

  (setup! [this test]
    (chu/exec-with-retries 30 (fn []
      (zk-create-if-not-exists conn root-path "" :with-acl (:with-auth test)))))

  (invoke! [this test op]
    (case (:f op)
      :enqueue (try
                 (do
                   (zk-create conn (concat-path root-path (:value op)) "" :with-acl (:with-auth test))
                   (assoc op :type :ok))
                 (catch Exception _ (assoc op :type :info, :error :connect-error)))
      :dequeue
      (try
        (let [result (zk-multi-delete-first-child conn root-path)]
          (if (not (nil? result))
            (assoc op :type :ok :value result)
            (assoc op :type :fail :value result)))
        (catch Exception _ (assoc op :type :info, :error :connect-error)))
      :drain
      ; drain via delete is to long, just list all nodes
      (chu/exec-with-retries 30 (fn []
                              (zk-sync conn)
                              (assoc op :type :ok :value (into #{} (map #(str %1) (zk-list conn root-path))))))))

  (teardown! [_ test])

  (close! [_ test]
    (zk/close conn)))

(defn sorted-str-range
  [n]
  (sort (map (fn [v] (str v)) (take n (range)))))

(defn total-workload
  "A generator, client, and checker for a set test."
  [opts]
  {:client    (QueueClient. nil nil)
   :checker   (checker/compose
               {:total-queue (checker/total-queue)
                :perf        (checker/perf)
                :timeline    (timeline/html)})
   :generator (->> (sorted-str-range 50000)
                   (map (fn [x]
                          (rand-nth [{:type :invoke, :f :enqueue :value x}
                                     {:type :invoke, :f :dequeue}]))))
   :final-generator (gen/once {:type :invoke, :f :drain, :value nil})})

(defn linear-workload
  [opts]
  {:client    (QueueClient. nil nil)
   :checker   (checker/compose
               {:linear   (checker/linearizable {:model     (model/unordered-queue)
                                                 :algorithm :linear})
                :perf        (checker/perf)
                :timeline (timeline/html)})
   :generator (->> (sorted-str-range 10000)
                   (map (fn [x]
                          (rand-nth [{:type :invoke, :f :enqueue :value x}
                                     {:type :invoke, :f :dequeue}]))))})

NTIRPC_MONITORING_${NTIRPC_VERSION_BASE} {
  global:
    # monitoring__*
    monitoring__register_counter;
    monitoring__register_gauge;
    monitoring__register_histogram;
    monitoring__counter_inc;
    monitoring__counter_get;
    monitoring__counter_set;
    monitoring__counter_remove;
    monitoring__gauge_inc;
    monitoring__gauge_dec;
    monitoring__gauge_set;
    monitoring__gauge_remove;
    monitoring__histogram_observe;
    monitoring__histogram_remove;
    monitoring__buckets_exp2;
    monitoring__buckets_exp2_compact;
    monitoring__get_registry_handle;

  local:
    *;
};

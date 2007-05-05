select dense_rank() over (partition by cn), cn, pn from sale; -- fail! dense_rank requires partitioning

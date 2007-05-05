select rank() over (partition by cn), cn, pn from sale; -- fail! rank requires partitioning

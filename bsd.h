#define IGMPV3_ALL_MCR htonl(INADDR_ALLRPTS_GROUP)
#define IGMPV3_HOST_MEMBERSHIP_REPORT IGMP_v3_HOST_MEMBERSHIP_REPORT

#define IGMPV3_CHANGE_TO_INCLUDE IGMP_CHANGE_TO_INCLUDE_MODE
#define IGMPV3_CHANGE_TO_EXCLUDE IGMP_CHANGE_TO_EXCLUDE_MODE

#define igmpv3_report igmp_report
#define ngrec ir_numgrps

#define igmpv3_grec igmp_grouprec
#define grec_type ig_type
#define grec_nsrcs ig_numsrc
#define grec_mca ig_group.s_addr

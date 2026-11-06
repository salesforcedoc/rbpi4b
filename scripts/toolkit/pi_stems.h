/* SPDX-License-Identifier: MIT */
/* Select the loaded track's own USB namespace; never search the other stick.
 * Pure bounded string handling, shared with native host tests. */
#ifndef RBPI4B_TOOLKIT_PI_STEMS_H
#define RBPI4B_TOOLKIT_PI_STEMS_H
static int rx3_pi_stem_path(const char *track, const char *directory,
                            char *output, unsigned int capacity)
{
    if (!track || !directory || !output || !capacity) return -1;
    unsigned int prefix = 0;
    const char *root = directory;
    if (directory[0]=='a' && directory[1]=='u' && directory[2]=='t' &&
        directory[3]=='o' && !directory[4]) {
        const char usb[] = "/media/usb";
        unsigned int i = 0;
        while (usb[i] && track[i] == usb[i]) i++;
        if (usb[i] || (track[i]!='1' && track[i]!='2') || track[i+1]!='/') return -1;
        unsigned int start = i + 2;
        i = start;
        while (track[i] && track[i]!='/') i++;
        if (i==start || track[i]!='/') return -1;
        root = track;
        prefix = i;
    } else {
        if (directory[0]!='/') return -1;
        while (directory[prefix]) prefix++;
        while (prefix && directory[prefix-1]=='/') prefix--;
    }
    const char *base = track, *dot = 0;
    for (const char *p = track; *p; p++) {
        if (*p=='/' || *p=='\\') { base=p+1;dot=0; }
        else if (*p=='.') dot=p;
    }
    if (!*base) return -1;
    const char *end = base;
    while (*end) end++;
    if (dot && dot>base) end=dot;
    unsigned int name_length = (unsigned int)(end-base);
    const char *middle = root==track ? "/RX3_STEMS/" : "/";
    unsigned int middle_length = root==track ? 11u : 1u;
    const char suffix[] = ".rx3stem";
    if (prefix>=capacity || middle_length>=capacity-prefix ||
        name_length>=capacity-prefix-middle_length ||
        sizeof(suffix)>capacity-prefix-middle_length-name_length) return -1;
    unsigned int at=0;
    for (unsigned int i=0;i<prefix;i++) output[at++]=root[i];
    for (unsigned int i=0;i<middle_length;i++) output[at++]=middle[i];
    for (unsigned int i=0;i<name_length;i++) output[at++]=base[i];
    for (unsigned int i=0;i<sizeof(suffix);i++) output[at++]=suffix[i];
    return 0;
}
#endif

import utils

# ----------------------------------------------------------------------------------------
def process_position(original, final):
    if original not in utils.expected_characters or final not in utils.expected_characters:
        raise Exception('one of %s %s not among expected characters' % (original, final))

    if original in utils.ambiguous_bases or final in utils.ambiguous_bases:
        return final

    if original != final:
        return utils.color('red', final)

    return final

# ----------------------------------------------------------------------------------------
def get_query_line(lseq, line, lengths, glseqs, indelfo=None):  # NOTE do not, on pain of death, modify <line>
    # build up the query sequence line, including colors for mutations and conserved codons
    j_right_extra = 0  # portion of query sequence to right of end of the j match
    n_inserted = 0
    final_seq_list = []
    if indelfo is not None:
        lastfo = indelfo['indels'][-1]  # if the "last" (arbitrary but necessary ordering) indel starts here
    for inuke in range(len(lseq)):
        # if we're at the position that the insertion started at (before we removed it)
        if indelfo is not None and lastfo['type'] == 'insertion':
            if inuke == lastfo['pos']:
                final_seq_list.append(lastfo['seqstr'])  # put the insertion back into the query sequence
                n_inserted += len(lastfo['seqstr'])
        if indelfo is not None and lastfo['type'] == 'deletion':
            if inuke - lastfo['pos'] >= 0 and inuke - lastfo['pos'] < lastfo['len']:  # if we're within the bases that we added to make up for the deletionlen
                final_seq_list.append(utils.color('light_blue', '*'))
                continue

        new_nuke = ''
        key = None
        ilocal = inuke
        if indelfo is not None:
            ilocal += n_inserted
        if ilocal < len(line['fv_insertion']):  # haven't got to start of v match yet, so just add on the query seq nuke
            pass
        else:
            ilocal -= len(line['fv_insertion'])
            if ilocal < lengths['v']:
                key = 'v'
            else:
                ilocal -= lengths['v']
                if ilocal < len(line['vd_insertion']):
                    key = 'vd_insertion'
                else:
                    ilocal -= len(line['vd_insertion'])
                    if ilocal < lengths['d']:
                        key = 'd'
                    else:
                        ilocal -= lengths['d']
                        if ilocal < len(line['dj_insertion']):
                            key = 'dj_insertion'
                        else:
                            ilocal -= len(line['dj_insertion'])
                            if ilocal < lengths['j']:
                                key = 'j'
                            else:
                                j_right_extra += 1

        if key is None:
            original = lseq[inuke]  # dummy value
        else:
            original = glseqs[key][ilocal] if key in glseqs else line[key][ilocal]
        new_nuke = process_position(original, lseq[inuke])

        for region, pos in line['codon_positions'].items():  # reverse video for the conserved codon positions
            if inuke >= pos and inuke < pos + 3:
                new_nuke = '\033[7m' + new_nuke + '\033[m'

        final_seq_list.append(new_nuke)

    return final_seq_list, j_right_extra

# ----------------------------------------------------------------------------------------
def handle_no_space(line, glseqs, final_seq_list):  # NOTE do not, on pain of death, modify <line>
    # if there isn't enough space for dots in the vj line, we add some blue dashes to everybody so things fit (very rare in heavy chain rearrangements, but pretty common in light chain)
    interior_length = len(line['vd_insertion']) + len(glseqs['d']) + len(line['dj_insertion'])  # length of the portion of the vj line that is normally taken up by dots (and spaces)
    if line['v_3p_del'] + line['j_5p_del'] > interior_length:  # not enough space
        v_3p_del_str = '.' + str(line['v_3p_del']) + '.'
        j_5p_del_str = '.' + str(line['j_5p_del']) + '.'
        extra_space_because_of_fixed_nospace = max(0, interior_length - len(v_3p_del_str + j_5p_del_str))

        gap_insertion_point = len(line['fv_insertion'] + glseqs['v'])
        gaps_to_add = len(v_3p_del_str + j_5p_del_str) - interior_length
        final_seq_list = final_seq_list[:gap_insertion_point] + gaps_to_add * [utils.color('blue', '-'), ] + final_seq_list[gap_insertion_point:]
    else:
        v_3p_del_str = '.' * line['v_3p_del']
        j_5p_del_str = '.' * line['j_5p_del']
        gaps_to_add = 0
        extra_space_because_of_fixed_nospace = 0

    return final_seq_list, utils.color('blue', '-') * gaps_to_add, v_3p_del_str, j_5p_del_str, extra_space_because_of_fixed_nospace

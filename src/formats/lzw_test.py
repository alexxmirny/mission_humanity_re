

def compress(uncompressed):
    """Compress a string to a list of output symbols."""

    # Build the dictionary.
    dict_size = 256
    dictionary = dict((chr(i), chr(i)) for i in range(0, dict_size))
    # in Python 3: dictionary = {chr(i): chr(i) for i in range(dict_size)}

    w = ""
    result = []
    for c in uncompressed:
        wc = w + c
        if wc in dictionary:
            w = wc
        else:
            result.append(dictionary[w])
            # Add wc to the dictionary.
            dictionary[wc] = dict_size
            dict_size += 1
            w = c

    # Output the code for w.
    if w:
        result.append(dictionary[w])
    return result


def decompress(compressed: bytearray):
    """Decompress a list of output ks to a string."""

    # Build the dictionary.
    dict_size = 256
    dictionary = dict((chr(i), chr(i)) for i in range(0, dict_size))
    # in Python 3: dictionary = {chr(i): chr(i) for i in range(dict_size)}
    w = result = chr(compressed.pop(0))
    for k in compressed:
        kx = chr(k)
        if kx in dictionary:
            entry = dictionary[kx]
        elif kx == dict_size:
            entry = w + w[0]
        else:
            raise ValueError('Bad compressed k: %s' % k)
        result += entry

        # Add w+entry[0] to the dictionary.
        dictionary[dict_size] = w + entry[0]
        dict_size += 1

        w = entry
    return result

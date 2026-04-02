(let [a "very"
      b "much"
      c "not"]
  (letfn [(neven? [n] (if (= n 0) [a b] (nodd? (- n 1))))
          (nodd? [n] (if (= n 0) [c a] (neven? (- n 1))))
          (peven? [n] (if (= n 0) "very much"
                                  (if (= 1 n) "not very"
                                              (peven? (- n 2)))))
          (alfa [] (beta))
          (beta [] 1)]
    [(nodd? 11) (peven? 12) (alfa)]))


#include "redisearch.h"
#include "spec.h"
#include "query.h"
#include "synonym_map.h"
#include "snowball/include/libstemmer.h"
#include "default.h"
#include "tokenize.h"
#include "rmutil/vector.h"
#include "stemmer.h"
#include "phonetic_manager.h"
#include "score_explain.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/param.h>

/////////////////////////////////////////////////////////////////////////////////////////////////////////
// TF-IDF Scoring Functions

// We have 2 TF-IDF scorers - one where TF is normalized by max frequency, the other where it is
// normalized by total weighted number of terms in the document

// normalize TF by max frequency
#define NORM_MAXFREQ 1

// normalize TF by number of tokens (weighted)
#define NORM_DOCLEN 2

#define EXPLAIN(fmt, args...)          \
  do {                                 \
    if (explain) {                     \
      explain->explain((fmt), ##args); \
    }                                  \
  } while(0)

void ScoreExplain::explain(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *p;
  rm_vasprintf((char ** __restrict) &p, fmt, ap);
  va_end(ap);

  str = p;
  rm_free(p);
}

//-------------------------------------------------------------------------------------------------------

ScorerArgs::ScorerArgs(const IndexSpec &spec, const SimpleBuff &ast_payload, bool bExplain) : indexStats(spec.stats) {
  explain = bExplain ? new ScoreExplain : NULL;
  payload = ast_payload;
}

//-------------------------------------------------------------------------------------------------------

ScorerArgs::~ScorerArgs() {
  delete explain;
}

//-------------------------------------------------------------------------------------------------------

ScoreExplain *ScorerArgs::CreateNewExplainParent() {
  if (!explain) {
    return NULL;
  }
  explain = new ScoreExplain(explain);
  return explain;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// recursively calculate tf-idf

double TermResult::TFIDFScorer(const RSDocumentMetadata *dmd, ScoreExplain *explain) const {
  double idf = term ? term->idf : 0;
  double score = weight * ((double)freq) * idf;
  EXPLAIN("(TFIDF %.2f = Weight %.2f * TF %d * IDF %.2f)", score, weight, freq, idf);
  return score;
}

//-------------------------------------------------------------------------------------------------------

double AggregateResult::TFIDFScorer(const RSDocumentMetadata *dmd, ScoreExplain *explain) const {
  double score = 0;
  if (!explain) {
    for (auto child : children) {
      score += child->TFIDFScorer(dmd, NULL);
    }
  } else {
    explain->children.clear();
    for (auto child: children) {
      ScoreExplain *exp;
      score += child->TFIDFScorer(dmd, exp);
      explain->children.push_back(exp);
    }
    EXPLAIN("(Weight %.2f * total children TFIDF %.2f)", weight, score);
  }
  return weight * score;
}

//-------------------------------------------------------------------------------------------------------

double IndexResult::TFIDFScorer(const RSDocumentMetadata *dmd, ScoreExplain *explain) const {
  EXPLAIN("(TFIDF %.2f = Weight %.2f * Frequency %d)", weight * (double)freq, weight, freq);
  return weight * (double)freq;
}

//-------------------------------------------------------------------------------------------------------

// internal common tf-idf function, where just the normalization method changes

double IndexResult::TFIDFScorer(const ScorerArgs *args, const RSDocumentMetadata *dmd, double minScore, int normMode) const {
  ScoreExplain *explain = args->explain;
  if (dmd->score == 0) {
    EXPLAIN("Document score is 0");
    return 0;
  }

  uint32_t norm = normMode == NORM_MAXFREQ ? dmd->maxFreq : dmd->len;
  double rawTfidf = TFIDFScorer(dmd, explain);
  double tfidf = dmd->score * rawTfidf / norm;

  explain = args->CreateNewExplainParent();

  // no need to factor the distance if tfidf is already below minimal score
  if (tfidf < minScore) {
    EXPLAIN("TFIDF score of %.2f is smaller than minimum score %.2f", tfidf, minScore);
    return 0;
  }

  int slop = MinOffsetDelta();
  tfidf /= slop;

  EXPLAIN("Final TFIDF : words TFIDF %.2f * document score %.2f / norm %d / slop %d",
          rawTfidf, dmd->score, norm, slop);

  return tfidf;
}

//-------------------------------------------------------------------------------------------------------

// Calculate sum(TF-IDF)*document score for each result, where TF is normalized by maximum frequency
// in this document.

double TFIDFScorer(const ScorerArgs *args, const IndexResult *result, const RSDocumentMetadata *dmd, double minScore) {
  return result->TFIDFScorer(args, dmd, minScore, NORM_MAXFREQ);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// Identical scorer to TFIDFScorer, only the normalization is by total weighted frequency in the doc

double TFIDFNormDocLenScorer(const ScorerArgs *args, const IndexResult *result, const RSDocumentMetadata *dmd, double minScore) {

  return result->TFIDFScorer(args, dmd, minScore, NORM_DOCLEN);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
// BM25 Scoring Functions
// https://en.wikipedia.org/wiki/Okapi_BM25

// recursively calculate score for each token, summing up sub tokens

double TermResult::BM25Scorer(const ScorerArgs *args, const RSDocumentMetadata *dmd) const {
  ScoreExplain *explain = args->explain;
  static const float b = 0.5;
  static const float k1 = 1.2;
  double f = (double)freq;
  double idf = (term ? term->idf : 0);

  double score = idf * f / (f + k1 * (1.0f - b + b * args->indexStats.avgDocLen));
  EXPLAIN("(%.2f = IDF %.2f * F %d / (F %d + k1 1.2 * (1 - b 0.5 + b 0.5 * Average Len %.2f)))",
          score, idf, freq, freq, args->indexStats.avgDocLen);

  return score;
}

//-------------------------------------------------------------------------------------------------------

double AggregateResult::BM25Scorer(const ScorerArgs *args, const RSDocumentMetadata *dmd) const {
  ScoreExplain *explain = args->explain;
  static const float b = 0.5;
  static const float k1 = 1.2;
  double f = (double)freq;
  double score = 0;

  if (!explain) {
    for (auto child : children) {
      score += child->BM25Scorer(args, dmd, NULL);
    }
  } else {
    explain->children.clear();
    for (auto child : children) {
      score += child->BM25Scorer(args, dmd);
      explain->children.push_back(child->explain); // @@@ check logic of collecting child explains. double free?
    }
    EXPLAIN("(Weight %.2f * children BM25 %.2f)", weight, score);
  }
  score *= weight;
  return score;
}

//-------------------------------------------------------------------------------------------------------

double IndexResult::BM25Scorer(const ScorerArgs *args, const RSDocumentMetadata *dmd) const {
  ScoreExplain *explain = args->explain;
  double f = (double)freq;
  double score = 0;

  if (f) { // default for virtual type - just disregard the idf
    static const float b = 0.5;
    static const float k1 = 1.2;
    score = weight * f / (f + k1 * (1.0f - b + b * args->indexStats.avgDocLen));
    EXPLAIN("(%.2f = Weight %.2f * F %d / (F %d + k1 1.2 * (1 - b 0.5 + b 0.5 * Average Len %.2f)))",
            score, weight, freq, freq, args->indexStats.avgDocLen);
  } else {
    EXPLAIN("Frequency 0 -> value 0");
  }

  return score;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
// BM25 scoring function

double BM25Scorer(ScorerArgs *args, const IndexResult *r, const RSDocumentMetadata *dmd, double minScore) {
  ScoreExplain *explain = args->explain;
  double bm25res = r->BM25Scorer(args, dmd);
  double score = dmd->score * bm25res;
  
  explain = args->CreateNewExplainParent();

  // no need to factor the distance if tfidf is already below minimal score
  if (score < minScore) {
    EXPLAIN("BM25 score of %.2f is smaller than minimum score %.2f", bm25res, score);
    return 0;
  }
  int slop = r->MinOffsetDelta();
  score /= slop;

  EXPLAIN("Final BM25 : words BM25 %.2f * document score %.2f / slop %d", bm25res, dmd->score, slop);

  return score;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
// Raw document-score scorer. Just returns the document score

double DocScoreScorer(const ScorerArgs *args, const IndexResult *r, const RSDocumentMetadata *dmd, double minScore) {
  ScoreExplain *explain = args->explain;
  EXPLAIN("Document's score is %.2f", dmd->score);
  return dmd->score;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
// DISMAX-style scorer

double IndexResult::DisMaxScorer(const ScorerArgs *args) const {
  ScoreExplain *explain = args->explain;
  // for terms - we return the term frequency
  double score = freq;
  EXPLAIN("DISMAX %.2f = Weight %.2f * Frequency %d", weight * score, weight, freq);

  return weight * score;
}

//-------------------------------------------------------------------------------------------------------

double IntersectResult::DisMaxScorer(const ScorerArgs *args) const {
  ScoreExplain *explain = args->explain;
  // for terms - we return the term frequency
  double score = 0;
  if (!explain) {
    for (auto child : children) {
      score += child->DisMaxScorer(args, NULL);
    }
  } else {
    explain->children.clear();
    for (auto child : children) {
      ScoreExplain *explain;
      score += child->DisMaxScorer(args, explain);
      explain->children.push_back(explain);
    }
    EXPLAIN("%.2f = Weight %.2f * children DISMAX %.2f", weight * score, weight, score);
  }

  return weight * score;
}

//-------------------------------------------------------------------------------------------------------

double UnionResult::DisMaxScorer(const ScorerArgs *args) const {
  ScoreExplain *explain = args->explain;
  double score = 0;
  if (!explain) {
    for (auto child : children) {
      score = MAX(score, child->DisMaxScorer(args, NULL));
    }
  } else {
    explain->children.clear();
    for (auto child : children) {
      ScoreExplain *exp;
      score = MAX(score, child->DisMaxScorer(args, exp));
      explain->children.push_back(exp);
    }
    EXPLAIN("%.2f = Weight %.2f * children DISMAX %.2f", weight * score, weight, score);
  }

  return weight * score;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// Calculate sum(TF-IDF)*document score for each result

double DisMaxScorer(const ScorerArgs *args, const IndexResult *h, const RSDocumentMetadata *dmd, double minScore) {
  return h->DisMaxScorer(args);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// taken from redis - bitops.c

static const unsigned char bitsinbyte[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};

//-------------------------------------------------------------------------------------------------------

// HAMMING - Scorer using Hamming distance between the query payload and the document payload.
// Only works if both have the payloads the same length

double HammingDistanceScorer(const ScorerArgs *args, const IndexResult *h, const RSDocumentMetadata *dmd, double minScore) {
  ScoreExplain *explain = args->explain;

  // the strings must be of the same length > 0
  if (!dmd->payload || !dmd->payload.length() || dmd->payload.length() != args->payload.lenth()) {
    EXPLAIN("Payloads provided to scorer vary in length");
    return 0; //@@@ TODO: is this a correct score?
  }

  size_t nbits = 0;
  size_t len = args->payload.len;
  const unsigned char *a = (unsigned char *)args->payload;
  const unsigned char *b = (unsigned char *)dmd->payload->data;
  for (size_t i = 0; i < len; ++i) {
    nbits += bitsinbyte[(unsigned char)(a[i] ^ b[i])];
  }

  // we inverse the distance, and add 1 to make sure a distance of 0 yields a perfect score of 1
  double score = 1.0 / (double)(nbits + 1);
  EXPLAIN("String length is %zu. Bit count is %zu. Result is (1 / count + 1) = %.2f", len, nbits, score);
  return score;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

void DefaultExpander::expandCn(RSToken *token) {
  if (!tokenizer) {
    tokenizer = new ChineseTokenizer(NULL, NULL, 0);
    tokens.clear();
  }

  tokenizer->Start(token->str.c_str(), token->length(), 0);

  Token tok;
  while (tokenizer->Next(&tok)) {
    tokens.emplace_back(tok.tok, tok.tokLen);
  }

  ExpandTokenWithPhrase(tokens, token->flags, true, false);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// Stemmer based query expander

int StemmerExpander::Expand(RSToken *token) {
  // we store the stemmer as private data on the first call to expand
  DefaultExpander *dd = ctx->privdata;
  struct sb_stemmer *sb;

  if (!ctx->privdata) {
    if (ctx->language == RS_LANG_CHINESE) {
      dd->expandCn(token);
      return REDISMODULE_OK;
    } else {
      dd = ctx->privdata = rm_calloc(1, sizeof(*dd));
      dd->isCn = false;
      sb = dd->latin = sb_stemmer_new(RSLanguage_ToString(ctx->language), NULL);
    }
  }

  if (dd->isCn) {
    dd->expandCn(token);
    return REDISMODULE_OK;
  }

  sb = dd->latin;

  // No stemmer available for this language - just return the node so we won't
  // be called again
  if (!sb) {
    return REDISMODULE_OK;
  }

  const sb_symbol *b = (const sb_symbol *)token->str;
  const sb_symbol *stemmed = sb_stemmer_stem(sb, b, token->length());

  if (stemmed) {
    int sl = sb_stemmer_length(sb);

    // Make a copy of the stemmed buffer with the + prefix given to stems
    char *dup = rm_malloc(sl + 2);
    dup[0] = STEM_PREFIX;
    memcpy(dup + 1, stemmed, sl + 1);
    dd->ExpandToken(dup, sl + 1, 0x0);  // TODO: Set proper flags here
    if (sl != token->length() || strncmp((const char *)stemmed, token->str.data(), token->length())) {
      dd->ExpandToken(rm_strndup((const char *)stemmed, sl), sl, 0x0);
    }
  }
  return REDISMODULE_OK;
}

//---------------------------------------------------------------------------------------------

StemmerExpander::StemmerExpander() {
  if (!p) {
    return;
  }
  DefaultExpander *dd = p;
  if (dd->isCn) {
    delete dd->tokenizer;
  } else if (dd->latin) {
    sb_stemmer_delete(dd->latin);
  }
  rm_free(dd);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// Phonetic-based query expander

int PhoneticExpander::Expand(RSToken *token) {
  char *primary = NULL;

  PhoneticManager::ExpandPhonetics(token->str.c_str(), token->length(), &primary, NULL);

  if (primary) {
    ExpandToken(primary, strlen(primary), 0x0);
  }
  return REDISMODULE_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// Synonyms-based query expander

#define BUFF_LEN 100

int SynonymExpander::Expand(RSToken *token) {
  IndexSpec *spec = sctx.spec;
  if (!spec->smap) {
    return REDISMODULE_OK;
  }

  TermData *t_data = spec->smap->GetIdsBySynonym(token->str, token->len);
  if (t_data == NULL) {
    return REDISMODULE_OK;
  }

  for (int i = 0; i < array_len(t_data->ids); ++i) {
    char buff[BUFF_LEN];
    int len = SynonymMap::IdToStr(t_data->ids[i], buff, BUFF_LEN);
    ExpandToken(rm_strdup((const char *)buff), len, 0x0);
  }
  return REDISMODULE_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// Default query expander

//@@ need to change the name - there is a struct with that name

int DefaultExpander::Expand(RSToken *token) {
  int phonetic = currentNode->opts.phonetic;
  SynonymExpander::Expand(token);

  if (phonetic == PHONETIC_DEFAULT) {
    // Eliminate the phonetic expansion if we know that none of the fields actually use phonetic matching
    if (sctx.spec->CheckPhoneticEnabled(currentNode->opts.fieldMask)) {
      phonetic = PHONETIC_ENABLED;
    }
  } else if (phonetic == PHONETIC_ENABLED || phonetic == PHONETIC_DESABLED) {
    // Verify that the field is actually phonetic
    bool isValid = false;
    if (currentNode->opts.fieldMask == RS_FIELDMASK_ALL) {
      if (sctx.spec->flags & Index_HasPhonetic) {
        isValid = true;
      }
    } else {
      t_fieldMask fm = currentNode->opts.fieldMask;
      for (size_t ii = 0; ii < sctx.spec->fields.size(); ++ii) {
        if (!(fm & (t_fieldMask)1 << ii)) {
          continue;
        }
        const FieldSpec fs = sctx.spec->fields[ii];
        if (fs.IsPhonetics()) {
          isValid = true;
        }
      }
    }
    if (!isValid) {
      status->SetError(QUERY_EINVAL, "field does not support phonetics");
      return REDISMODULE_ERR;
    }
  }
  if (phonetic == PHONETIC_ENABLED) {
    PhoneticExpander::Expand(token);
  }

  // stemmer is happenning last because it might free the given 'RSToken *token'
  // this is a bad solution and should be fixed, but for now its good enough
  // @@TODO: fix the free of the 'RSToken *token' by the stemmer and allow any expnders ordering!!
  StemmerExpander::Expand(token);
  return REDISMODULE_OK;
}

//---------------------------------------------------------------------------------------------

DefaultExpander::~DefaultExpander() {
  StemmerExpanderFree(p);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

// Register the default extension

DefaultExtension::DefaultExtension() {
  // TF-IDF scorer
  Register(DEFAULT_SCORER_NAME, TFIDFScorer);

  // DisMax-alike scorer
  Register(DISMAX_SCORER_NAME, DisMaxScorer);

  // BM25 scorer
  Register(BM25_SCORER_NAME, BM25Scorer);

  // HAMMING scorer
  Register(HAMMINGDISTANCE_SCORER_NAME, HammingDistanceScorer);

  // TFIDF.DOCNORM
  Register(TFIDF_DOCNORM_SCORER_NAME, TFIDFNormDocLenScorer);

  // DOCSCORE scorer
  Register(DOCSCORE_SCORER_NAME, DocScoreScorer);

  // Snowball Stemmer expander
  Register(STEMMER_EXPENDER_NAME, StemmerExpander);

  // Synonyms expander
  Register(SYNONYMS_EXPENDER_NAME, SynonymExpand);

  // Phonetic expander
  Register(PHONETIC_EXPENDER_NAME, PhoneticExpand);

  // Default expander
  Register(DEFAULT_EXPANDER_NAME, DefaultExpander);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

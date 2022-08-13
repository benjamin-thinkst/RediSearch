#include "example.h"
#include "redisearch.h"

#include <string.h>
#include <stdio.h>
#include <sys/param.h>

///////////////////////////////////////////////////////////////////////////////////////////////

// Calculate sum(TF-IDF)*document score for each result

static double myScorer(const ScorerArgs *args, const IndexResult *h, const RSDocumentMetadata *dmd, double minScore) {
  return 3.141;
}

//---------------------------------------------------------------------------------------------

static double filterOutScorer(const ScorerArgs *args, const IndexResult *h, const RSDocumentMetadata *dmd, double minScore) {
  return RS_SCORE_FILTEROUT;
}

//---------------------------------------------------------------------------------------------

int myExpander(QueryExpander *expander, RSToken *token) {
  expander->ExpandToken(expander, strdup("foo"), 3, 0x00ff);
  return REDISEARCH_OK;
}

//---------------------------------------------------------------------------------------------

int numFreed = 0;

void myFreeFunc(void *p) {
  // printf("Freeing %p\n", p);
  numFreed++;
  free(p);
}

//---------------------------------------------------------------------------------------------

// Register the default extension

int RS_ExtensionInit(RSExtensionCtx *ctx) {
  if (ctx->RegisterScoringFunction("example_scorer", myScorer, myFreeFunc, NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  if (ctx->RegisterScoringFunction("filterout_scorer", filterOutScorer, myFreeFunc, NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  // Snowball Stemmer is the default expander
  if (ctx->RegisterQueryExpander("example_expander", myExpander, myFreeFunc, NULL) == REDISEARCH_ERR) {
    return REDISEARCH_ERR;
  }

  return REDISEARCH_OK;
}

///////////////////////////////////////////////////////////////////////////////////////////////

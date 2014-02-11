
#include <mitie.h>

#include <mitie/named_entity_extractor.h>
#include <mitie/ner_feature_extraction.h>

#include <map>
#include <iostream>
#include <dlib/cmd_line_parser.h>

#include "conll_parser.h"

#include <dlib/svm_threaded.h>
#include <mitie/total_word_feature_extractor.h>
#include <mitie/stemmer.h>
#include <mitie/unigram_tokenizer.h>



using namespace dlib;
using namespace std;
using namespace mitie;

// ----------------------------------------------------------------------------------------

void train_chunker(const command_line_parser& parser);
void test_chunker(const command_line_parser& parser);
void train_id(const command_line_parser& parser);
void test_id(const command_line_parser& parser);
void tag_file(const command_line_parser& parser);
void tag_conll_file(const command_line_parser& parser);

// ----------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    try
    {
        command_line_parser parser;
        parser.add_option("h", "Display this help information.");
        parser.add_option("train-chunker", "train NER chunker on conll data.");
        parser.add_option("test-chunker", "test NER chunker on conll data.");
        parser.add_option("train-id", "train NER ID/classification on conll data.");
        parser.add_option("test-id", "test NER ID/classification on conll data.");
        parser.add_option("C", "Set SVM C parameter to <arg> (default 100.0).",1);
        parser.add_option("eps", "Set SVM stopping epsilon parameter to <arg> (default 0.1).",1);
        parser.add_option("threads", "Use <arg> threads when doing training (default: 4).",1);
        parser.add_option("cache-size", "Set the max cutting plane cache size to <arg> (default: 5).",1);

        parser.add_option("tag-file", "Read in a text file and tag it with the ner model in file <arg>.",1);
        parser.add_option("tag-conll-file", "Read in a CoNLL annotation file and output a copy that is tagged with the NER model from the file <arg>.",1);

        parser.parse(argc,argv);
        parser.check_option_arg_range("C", 1e-9, 1e9);
        parser.check_option_arg_range("threads", 1, 64);
        parser.check_option_arg_range("cache-size", 0, 500);

        const char* training_ops[] = {"train-chunker", "train-id"};
        const char* training_subops[] = {"C", "eps", "threads", "cache-size"};
        parser.check_sub_options(training_ops, training_subops);

        if (parser.option("h"))
        {
            cout << "Usage: ner [options]\n";
            parser.print_options(); 
            return 0;
        }

        if (parser.option("tag-file"))
        {
            tag_file(parser);
            return 0;
        }

        if (parser.option("tag-conll-file"))
        {
            tag_conll_file(parser);
            return 0;
        }

        if (parser.option("train-chunker"))
        {
            train_chunker(parser);
            return 0;
        }

        if (parser.option("test-chunker"))
        {
            test_chunker(parser);
            return 0;
        }

        if (parser.option("train-id"))
        {
            train_id(parser);
            return 0;
        }

        if (parser.option("test-id"))
        {
            test_id(parser);
            return 0;
        }

        return 0;
    }
    catch (std::exception& e)
    {
        cout << e.what() << endl;
        return 1;
    }
}

// ----------------------------------------------------------------------------------------

std::string get_mitie_models_path()
{
    const char* models = getenv("MITIE_MODELS");
    if (models==0)
        throw dlib::error("MITIE_MODELS environment variable not set.  It should contain the path to the MITIE-models repository.");
    return models;
}

void train_chunker(const command_line_parser& parser)
{
    std::vector<std::vector<std::string> > sentences;
    std::vector<std::vector<std::pair<unsigned long, unsigned long> > > chunks;
    std::vector<std::vector<unsigned long> > chunk_labels;
    parse_conll_data(parser[0], sentences, chunks, chunk_labels);
    cout << "number of sentences loaded: "<< sentences.size() << endl;

    const std::string models_path = get_mitie_models_path();

    total_word_feature_extractor fe;
    std::ifstream fin((models_path + "/total_word_feature_extractor.dat").c_str(), ios::binary);
    deserialize(fe, fin);

    cout << "words in dictionary: " << fe.get_num_words_in_dictionary() << endl;
    cout << "num features: " << fe.get_num_dimensions() << endl;


    // do the feature extraction for all the sentences
    std::vector<std::vector<matrix<float,0,1> > > samples;
    samples.reserve(sentences.size());
    for (unsigned long i = 0; i < sentences.size(); ++i)
    {
        samples.push_back(sentence_to_feats(fe, sentences[i]));
    }

    cout << "now do training" << endl;

    ner_feature_extractor nfe(fe.get_num_dimensions());
    structural_sequence_segmentation_trainer<ner_feature_extractor> trainer(nfe);

    const double C = get_option(parser, "C", 15.0);
    const double eps = get_option(parser, "eps", 0.01);
    const unsigned long num_threads = get_option(parser, "threads", 4);
    const unsigned long cache_size = get_option(parser, "cache-size", 5);
    cout << "C:           "<< C << endl;
    cout << "epsilon:     "<< eps << endl;
    cout << "num threads: "<< num_threads << endl;
    cout << "cache size:  "<< cache_size << endl;
    trainer.set_c(C);
    trainer.set_epsilon(eps);
    trainer.set_num_threads(num_threads);
    trainer.set_max_cache_size(cache_size);
    trainer.be_verbose();

    sequence_segmenter<ner_feature_extractor> segmenter = trainer.train(samples, chunks);

    cout << "num feats in chunker model: "<< segmenter.get_weights().size() << endl;
    cout << "precision, recall, f1-score: "<< test_sequence_segmenter(segmenter, samples, chunks) << endl;

    ofstream fout("trained_segmenter.dat", ios::binary);
    serialize(fe, fout);
    serialize(segmenter, fout);
}

// ----------------------------------------------------------------------------------------

void test_chunker(const command_line_parser& parser)
{
    std::vector<std::vector<std::string> > sentences;
    std::vector<std::vector<std::pair<unsigned long, unsigned long> > > chunks;
    std::vector<std::vector<unsigned long> > chunk_labels;
    parse_conll_data(parser[0], sentences, chunks, chunk_labels);
    cout << "number of sentences loaded: "<< sentences.size() << endl;

    ifstream fin("trained_segmenter.dat", ios::binary);
    total_word_feature_extractor fe;
    sequence_segmenter<ner_feature_extractor> segmenter;
    deserialize(fe, fin);
    deserialize(segmenter, fin);

    std::vector<std::vector<matrix<float,0,1> > > samples;
    samples.reserve(sentences.size());
    for (unsigned long i = 0; i < sentences.size(); ++i)
    {
        samples.push_back(sentence_to_feats(fe, sentences[i]));
    }

    cout << "precision, recall, f1-score: "<< test_sequence_segmenter(segmenter, samples, chunks) << endl;
}

// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------


// ----------------------------------------------------------------------------------------

unsigned long get_label (
    const std::vector<std::pair<unsigned long, unsigned long> >& chunks,
    const std::vector<unsigned long>& chunk_labels,
    const std::pair<unsigned long, unsigned long>& range
)
/*!
    requires
        - chunks.size() == chunk_labels.size()
    ensures
        - This function checks if any of the elements of chunks are equal to range.  If so,
          then the corresponding chunk label is returned.  Otherwise a value of NOT_ENTITY
          is returned.
!*/
{
    for (unsigned long i = 0; i < chunks.size(); ++i)
    {
        if (range == chunks[i])
            return chunk_labels[i];
    }
    return NOT_ENTITY;
}

// ----------------------------------------------------------------------------------------

void train_id(const command_line_parser& parser)
{
    std::vector<std::vector<std::string> > sentences;
    std::vector<std::vector<std::pair<unsigned long, unsigned long> > > chunks;
    std::vector<std::vector<unsigned long> > chunk_labels;
    parse_conll_data(parser[0], sentences, chunks, chunk_labels);
    cout << "number of sentences loaded: "<< sentences.size() << endl;


    ifstream fin("trained_segmenter.dat", ios::binary);
    total_word_feature_extractor fe;
    sequence_segmenter<ner_feature_extractor> segmenter;
    deserialize(fe, fin);
    deserialize(segmenter, fin);

    std::vector<ner_sample_type> samples;
    std::vector<unsigned long> labels;
    for (unsigned long i = 0; i < sentences.size(); ++i)
    {
        const std::vector<matrix<float,0,1> >& sent = sentence_to_feats(fe, sentences[i]);
        std::set<std::pair<unsigned long, unsigned long> > ranges;
        // put all the true chunks into ranges
        ranges.insert(chunks[i].begin(), chunks[i].end());

        // now get all the chunks our segmenter finds
        std::vector<std::pair<unsigned long, unsigned long> > temp;
        temp = segmenter(sent);
        ranges.insert(temp.begin(), temp.end());

        // now go over all the chunks we found and label them with their appropriate NER
        // types and also do feature extraction for each.
        std::set<std::pair<unsigned long,unsigned long> >::const_iterator j;
        for (j = ranges.begin(); j != ranges.end(); ++j)
        {
            samples.push_back(extract_ner_chunk_features(sentences[i], sent, *j));
            labels.push_back(get_label(chunks[i], chunk_labels[i], *j));
        }
    }

    cout << "now do training" << endl;
    cout << "num training samples: " << samples.size() << endl;

    svm_multiclass_linear_trainer<sparse_linear_kernel<ner_sample_type>,unsigned long> trainer;

    const double C = get_option(parser, "C", 450.0);
    const double eps = get_option(parser, "eps", 0.001);
    const unsigned long num_threads = get_option(parser, "threads", 4);
    cout << "C:           "<< C << endl;
    cout << "epsilon:     "<< eps << endl;
    cout << "num_threads: "<< num_threads << endl;
    trainer.set_c(C);
    trainer.set_epsilon(eps);
    trainer.be_verbose();
    trainer.set_num_threads(num_threads);

    randomize_samples(samples, labels);
    /*
    matrix<double> res = cross_validate_multiclass_trainer(trainer, samples, labels, 5);
    cout << "5-fold cross-validation: \n" << res << endl;
    cout << "overall accuracy: "<< sum(diag(res))/sum(res) << endl;
    */

    multiclass_linear_decision_function<sparse_linear_kernel<ner_sample_type>,unsigned long> df;
    df = trainer.train(samples, labels);
    matrix<double> res = test_multiclass_decision_function(df, samples, labels);
    cout << "test on train: \n" << res << endl;
    cout << "overall accuracy: "<< sum(diag(res))/sum(res) << endl;

    cout << "C:           "<< C << endl;
    cout << "epsilon:     "<< eps << endl;
    cout << "num_threads: "<< num_threads << endl;

    std::vector<std::string> ner_labels(4);
    ner_labels[PER] = "PERSON";
    ner_labels[LOC] = "LOCATION";
    ner_labels[ORG] = "ORGANIZATION";
    ner_labels[MISC] = "MISC";
    named_entity_extractor ner(ner_labels, fe, segmenter, df);
    ofstream fout("ner_model.dat", ios::binary);
    serialize(ner, fout);
}

// ----------------------------------------------------------------------------------------

void test_id(const command_line_parser& parser)
{
    named_entity_extractor ner;
    ifstream fin("ner_model.dat", ios::binary);
    deserialize(ner, fin);

    std::vector<std::vector<std::string> > sentences;
    std::vector<std::vector<std::pair<unsigned long, unsigned long> > > chunks;
    std::vector<std::vector<unsigned long> > chunk_labels;
    parse_conll_data(parser[0], sentences, chunks, chunk_labels);
    cout << "number of sentences loaded: "<< sentences.size() << endl;


    const unsigned long num_labels = ner.get_tag_name_strings().size();
    std::vector<double> num_targets(num_labels);
    std::vector<double> num_dets(num_labels);
    std::vector<double> num_true_dets(num_labels);

    std::vector<std::pair<unsigned long, unsigned long> > ranges;
    std::vector<unsigned long> predicted_labels;

    for (unsigned long i = 0; i < sentences.size(); ++i)
    {
        ner(sentences[i], ranges, predicted_labels);

        for (unsigned long j = 0; j < ranges.size(); ++j)
        {
            const unsigned long predicted_label = predicted_labels[j];
            const unsigned long true_label = get_label(chunks[i], chunk_labels[i], ranges[j]);

            num_dets[predicted_label]++;
            if (predicted_label == true_label)
                num_true_dets[true_label]++;
        }
        for (unsigned long j = 0; j < chunk_labels[i].size(); ++j)
        {
            num_targets[chunk_labels[i][j]]++;
        }
    }

    cout << "results: "<< endl;
    for (unsigned long i = 0; i < num_targets.size(); ++i)
    {
        cout << "label: "<< i << endl;
        double prec = num_true_dets[i]/num_dets[i];
        double recall = num_true_dets[i]/num_targets[i];
        cout << "   precision: "<< prec << endl;
        cout << "   recall:    "<< recall << endl;
        cout << "   f1:        "<< 2*prec*recall/(prec+recall) << endl;
        cout << endl;
    }

    cout << "total: " << endl;
    double prec = sum(mat(num_true_dets))/sum(mat(num_dets));
    double recall = sum(mat(num_true_dets))/sum(mat(num_targets));
    cout << "   precision: "<< prec << endl;
    cout << "   recall:    "<< recall << endl;
    cout << "   f1:        "<< 2*prec*recall/(prec+recall) << endl;

}

// ----------------------------------------------------------------------------------------



// ----------------------------------------------------------------------------------------

void tag_file(const command_line_parser& parser)
{

    string ner_model = parser.option("tag-file").argument();
    mitie_named_entity_extractor* ner = mitie_load_named_entity_extractor(ner_model.c_str());
    if (ner == NULL)
    {
        cout << "couldn't load model file" << endl;
        return;
    }

    ifstream fin(parser[0].c_str());
    ostringstream sout;
    sout << fin.rdbuf();
    const string text = sout.str();

    const unsigned long num_tags = mitie_get_num_possible_ner_tags(ner);
    cout << "NER tags: "<< num_tags << endl;
    for(unsigned long i = 0; i < num_tags; ++i)
        cout << "   " << mitie_get_named_entity_tagstr(ner, i) << endl;

    mitie_named_entity_detections* dets = mitie_extract_entities(ner, text.c_str());

    const unsigned long num_dets = mitie_ner_get_num_detections(dets);
    cout << "num_dets: "<< num_dets << endl;
    for (unsigned long i = 0; i < num_dets; ++i)
    {
        const unsigned long begin = mitie_ner_get_detection_position(dets, i);
        const unsigned long len = mitie_ner_get_detection_length(dets, i);
        string temp(&text[begin], &text[begin]+len);

        cout << "   " << mitie_ner_get_detection_tagstr(dets,i) << "("<< 
                         mitie_ner_get_detection_tag(dets,i) << "),  " << temp << endl;

    }


    mitie_free(dets);
    mitie_free(ner);

    /*
    string ner_model = parser.option("tag-file").argument();
    ifstream fin(ner_model.c_str(), ios::binary);
    named_entity_extractor ner;
    deserialize(ner, fin);

    fin.close();
    fin.open(parser[0].c_str());

    unigram_tokenizer tok(fin);

    std::vector<std::string> words;
    string word;
    while(tok(word))
        words.push_back(word);

    std::vector<std::pair<unsigned long, unsigned long> > ranges;
    std::vector<unsigned long> predicted_labels;
    std::vector<std::string> tags = ner.get_tag_name_strings();
    tags.push_back("O");
    ner(words, ranges, predicted_labels);

    std::vector<unsigned long> word_tags(words.size(), tags.size()-1);
    for (unsigned long i = 0; i < ranges.size(); ++i)
    {
        for (unsigned long j = ranges[i].first; j < ranges[i].second; ++j)
        {
            word_tags[j] = predicted_labels[i];
        }
    }

    for (unsigned long i = 0; i < words.size(); ++i)
    {
        cout << words[i] << "/" << tags[word_tags[i]] << " ";
    }
    */

}

// ----------------------------------------------------------------------------------------

void tag_conll_file(const command_line_parser& parser)
{
    string ner_model = parser.option("tag-conll-file").argument();
    ifstream fin(ner_model.c_str(), ios::binary);
    named_entity_extractor ner;
    deserialize(ner, fin);


    std::vector<labeled_sentence> conll_data = parse_conll_data (parser[0]);
    std::vector<std::vector<std::string> > tokens;
    std::vector<std::vector<BIO_label> > labels;
    separate_labels_from_tokens(conll_data, tokens, labels);

    std::vector<std::pair<unsigned long, unsigned long> > ranges;
    std::vector<unsigned long> predicted_labels;
    for (unsigned long i = 0; i < tokens.size(); ++i)
    {
        ner(tokens[i], ranges, predicted_labels);
        labels[i].assign(labels[i].size(),O);
        for (unsigned long j = 0; j < ranges.size(); ++j)
        {
            for (unsigned long k = ranges[j].first; k < ranges[j].second; ++k)
            {
                if (j > 0 && ranges[j].first == ranges[j-1].second && predicted_labels[j] == predicted_labels[j-1])
                {
                    if (predicted_labels[j] == PER)
                        labels[i][k] = B_PER; 
                    else if (predicted_labels[j] == ORG)
                        labels[i][k] = B_ORG; 
                    else if (predicted_labels[j] == LOC)
                        labels[i][k] = B_LOC; 
                    else if (predicted_labels[j] == MISC)
                        labels[i][k] = B_MISC; 
                }
                else
                {
                    if (predicted_labels[j] == PER)
                        labels[i][k] = I_PER; 
                    else if (predicted_labels[j] == ORG)
                        labels[i][k] = I_ORG; 
                    else if (predicted_labels[j] == LOC)
                        labels[i][k] = I_LOC; 
                    else if (predicted_labels[j] == MISC)
                        labels[i][k] = I_MISC; 
                }
            }
        }
    }

    print_conll_data(conll_data, labels);
}

// ----------------------------------------------------------------------------------------
